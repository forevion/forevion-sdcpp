#include "dgl/ops/kandinsky6_vsr.h"

#include <algorithm>
#include <cfenv>
#include <cmath>
#include <stdexcept>
#include <string>

namespace sd::dgl::ops {
    namespace {
        constexpr int64_t SNAP = 16;  // K-VAE spatial factor

        // Python's round(): halves go to the even neighbour.
        int64_t round_even(double value) {
            const int mode = std::fegetround();
            std::fesetround(FE_TONEAREST);
            const double rounded = std::nearbyint(value);
            std::fesetround(mode);
            return static_cast<int64_t>(rounded);
        }

        // Evenly spread, SNAP-aligned tile starts covering [0, length - tile] with at least
        // min_overlap of a tile shared between neighbours (Kandinsky6SRPipeline._tile_positions).
        std::vector<int64_t> tile_positions(int64_t length, int64_t tile, double min_overlap) {
            if (tile >= length)
                return {0};
            const int64_t span       = (length - tile) / SNAP;
            const int64_t max_stride = std::max<int64_t>(1, int64_t(std::floor(double(tile) * (1.0 - min_overlap) / SNAP)));
            const int64_t count      = int64_t(std::ceil(double(span) / double(max_stride))) + 1;
            std::vector<int64_t> positions;
            for (int64_t i = 0; i < count; ++i)
                positions.push_back(round_even(double(i) * double(span) / double(count - 1)) * SNAP);
            return positions;
        }

        // torch.hann_window(n + 2)[1:-1]: periodic Hann without its zero endpoints.
        std::vector<float> hann(int64_t n) {
            std::vector<float> window(static_cast<size_t>(n));
            const double length = double(n + 2);
            for (int64_t i = 0; i < n; ++i)
                window[static_cast<size_t>(i)] = float(0.5 - 0.5 * std::cos(2.0 * M_PI * double(i + 1) / length));
            return window;
        }
    }

    Kandinsky6VSRPlan kandinsky6_vsr_plan(int64_t width, int64_t height, double scale, double min_overlap) {
        if (scale != 2.0 && scale != 2.25 && scale != 4.0)
            throw std::runtime_error("Kandinsky 6 VSR scale must be 2, 2.25 or 4");
        if (!(min_overlap >= 0.0 && min_overlap < 1.0))
            throw std::runtime_error("min_overlap must be in [0, 1)");
        Kandinsky6VSRPlan plan;
        plan.factor = scale == 4.0 ? 4 : 2;
        plan.width  = width / SNAP * SNAP;
        plan.height = height / SNAP * SNAP;
        if (scale == 2.25) {
            // 2.25x = a 1.125x bilinear pre-upscale followed by the 2x path.
            plan.width  = round_even(double(plan.width) * 1.125 / SNAP) * SNAP;
            plan.height = round_even(double(plan.height) * 1.125 / SNAP) * SNAP;
        }
        // Trained output tile sizes (height, width); the one closest to the video's aspect wins.
        const std::array<std::array<int64_t, 2>, 3> sizes{{{512, 512}, {512, 768}, {768, 512}}};
        const double aspect = double(plan.width) / double(std::max<int64_t>(1, plan.height));
        auto best           = sizes[0];
        for (const auto& size : sizes)
            if (std::abs(double(size[1]) / double(size[0]) - aspect) < std::abs(double(best[1]) / double(best[0]) - aspect))
                best = size;
        plan.tile_height = best[0] / plan.factor;
        plan.tile_width  = best[1] / plan.factor;
        if (plan.width < plan.tile_width || plan.height < plan.tile_height)
            throw std::runtime_error("Kandinsky 6 VSR needs at least " + std::to_string(plan.tile_width) + "x" + std::to_string(plan.tile_height) +
                                     " input pixels at this scale, got " + std::to_string(plan.width) + "x" + std::to_string(plan.height));
        for (int64_t top : tile_positions(plan.height, plan.tile_height, min_overlap))
            for (int64_t left : tile_positions(plan.width, plan.tile_width, min_overlap))
                plan.tiles.push_back({left, top});
        return plan;
    }

    Tensor<uint8_t> kandinsky6_vsr_blend(const std::vector<Tensor<float>>& tiles, const Kandinsky6VSRPlan& plan) {
        if (tiles.size() != plan.tiles.size() || tiles.empty())
            throw std::runtime_error("Kandinsky 6 VSR blend needs one decoded tile per planned tile");
        const int64_t f = plan.factor, tw = plan.tile_width * f, th = plan.tile_height * f;
        const int64_t W = plan.width * f, H = plan.height * f;
        const int64_t T = tiles[0].shape().size() > 2 ? tiles[0].shape()[2] : 1;
        const auto wx = hann(tw), wy = hann(th);
        std::vector<float> acc(size_t(3 * W * H * T), 0.f), weight(size_t(W * H), 0.f);
        for (size_t i = 0; i < tiles.size(); ++i) {
            const auto& tile = tiles[i];
            const auto& s    = tile.shape();
            if (s.size() < 4 || s[0] != tw || s[1] != th || s[2] != T || s[3] != 3)
                throw std::runtime_error("Kandinsky 6 VSR decoded tiles must be f32 [" + std::to_string(tw) + ", " + std::to_string(th) + ", T, 3]");
            const int64_t x0 = plan.tiles[i][0] * f, y0 = plan.tiles[i][1] * f;
            for (int64_t y = 0; y < th; ++y)
                for (int64_t x = 0; x < tw; ++x)
                    weight[size_t((y0 + y) * W + x0 + x)] += wy[size_t(y)] * wx[size_t(x)];
            for (int64_t c = 0; c < 3; ++c)
                for (int64_t t = 0; t < T; ++t)
                    for (int64_t y = 0; y < th; ++y) {
                        const float* src = tile.data() + ((c * T + t) * th + y) * tw;
                        float* dst       = acc.data() + ((t * H + y0 + y) * W + x0) * 3 + c;
                        const float wrow = wy[size_t(y)];
                        for (int64_t x = 0; x < tw; ++x)
                            dst[x * 3] += src[x] * wrow * wx[size_t(x)];
                    }
        }
        // Video frames are U8 [3, W, H, T].
        Tensor<uint8_t> output({3, W, H, T});
        for (int64_t t = 0; t < T; ++t)
            for (int64_t p = 0; p < W * H; ++p)
                for (int64_t c = 0; c < 3; ++c) {
                    const float v = std::clamp(acc[size_t((t * W * H + p) * 3 + c)] / weight[size_t(p)], 0.f, 1.f);
                    output.data()[(t * W * H + p) * 3 + c] = uint8_t(std::lround(v * 255.f));
                }
        return output;
    }
}  // namespace sd::dgl::ops
