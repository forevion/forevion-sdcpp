#ifndef __SD_DGL_OPS_KANDINSKY6_VSR_H__
#define __SD_DGL_OPS_KANDINSKY6_VSR_H__

#include <array>
#include <vector>

#include "core/tensor.hpp"

namespace sd::dgl::ops {
    struct Kandinsky6VSRPlan {
        int64_t width = 0, height = 0;            // input size after the optional pre-upscale
        int factor    = 2;                         // spatial factor of the latent upscaler and DiT tiles
        int64_t tile_width = 0, tile_height = 0;  // input-space tile size
        std::vector<std::array<int64_t, 2>> tiles; // input-space (x, y) of every tile
    };

    Kandinsky6VSRPlan kandinsky6_vsr_plan(int64_t width, int64_t height, double scale, double min_overlap);
    Tensor<uint8_t> kandinsky6_vsr_blend(const std::vector<Tensor<float>>& tiles, const Kandinsky6VSRPlan& plan);
}  // namespace sd::dgl::ops

#endif  // __SD_DGL_OPS_KANDINSKY6_VSR_H__
