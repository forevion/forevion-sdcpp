#ifndef __SD_MODEL_UPSCALER_KANDINSKY6_LATENT_UPSCALER_HPP__
#define __SD_MODEL_UPSCALER_KANDINSKY6_LATENT_UPSCALER_HPP__

#include "core/ggml_graph_cut.h"
#include "model/vae/seedvr2_vae.hpp"
#include "model/vae/wan_vae.hpp"

// Latent upscalers of the Kandinsky 6.0 VSR models (Kandinsky6SRLatentUpscalerBank in diffusers):
// convolutional networks that turn K-VAE latents into latents at 2x or 4x the spatial size, with
// every norm modulated by the input latent. Bank entry 0 is the x2 model, entry 1 the x4 model.
// The x2 entry also stores a backbone its forward never reads; it is not loaded.
namespace Kandinsky6Upscaler {
    // Conv3d that repeats the edge frames along time and zero-pads height and width.
    class ReplicateConv : public SeedVR2::InflatedCausalConv3d {
        int pad;

    public:
        ReplicateConv(int in, int out, int kernel)
            : SeedVR2::InflatedCausalConv3d(in, out, std::tuple<int, int, int>{kernel, kernel, kernel}, std::tuple<int, int, int>{1, 1, 1},
                                            std::tuple<int, int, int>{0, kernel / 2, kernel / 2}),
              pad(kernel / 2) {}

        ggml_tensor* forward(GGMLRunnerContext* ctx, ggml_tensor* x) override {
            auto c = ctx->ggml_ctx;
            if (pad > 0) {
                const int64_t T = x->ne[2];
                auto edge       = [&](int64_t frame) {
                    auto f = ggml_ext_slice(ctx, x, 2, frame, frame + 1);
                    return ggml_repeat(c, f, ggml_new_tensor_4d(c, x->type, x->ne[0], x->ne[1], pad, x->ne[3]));
                };
                x = ggml_concat(c, ggml_concat(c, edge(0), x, 2), edge(T - 1), 2);
            }
            return SeedVR2::InflatedCausalConv3d::forward(ctx, x);
        }
    };

    static std::shared_ptr<SeedVR2::InflatedCausalConv3d> plain_conv(int in, int out, std::tuple<int, int, int> kernel) {
        return std::make_shared<SeedVR2::InflatedCausalConv3d>(in, out, kernel, std::tuple<int, int, int>{1, 1, 1},
                                                               std::tuple<int, int, int>{std::get<0>(kernel) / 2, std::get<1>(kernel) / 2, std::get<2>(kernel) / 2});
    }

    static ggml_tensor* apply(GGMLRunnerContext* ctx, const std::shared_ptr<GGMLBlock>& block, ggml_tensor* x) {
        return std::dynamic_pointer_cast<UnaryBlock>(block)->forward(ctx, x);
    }

    static ggml_tensor* resize(GGMLRunnerContext* ctx, ggml_tensor* z, ggml_tensor* like) {
        if (z->ne[0] == like->ne[0] && z->ne[1] == like->ne[1] && z->ne[2] == like->ne[2])
            return z;
        return ggml_interpolate(ctx->ggml_ctx, ggml_ext_cont(ctx, z), like->ne[0], like->ne[1], like->ne[2], z->ne[3], GGML_SCALE_MODE_NEAREST);
    }

    class ModulatedNorm : public GGMLBlock {
    public:
        ModulatedNorm(int channels, int zq_channels) {
            blocks["norm"]   = std::make_shared<WAN::RMS_norm>(channels);
            blocks["conv_y"] = plain_conv(zq_channels, channels, {1, 1, 1});
            blocks["conv_b"] = plain_conv(zq_channels, channels, {1, 1, 1});
        }

        ggml_tensor* forward(GGMLRunnerContext* ctx, ggml_tensor* x, ggml_tensor* zq) {
            // Pointwise convolutions commute with the nearest resize, so they run at input size.
            auto y = resize(ctx, apply(ctx, blocks["conv_y"], zq), x);
            auto b = resize(ctx, apply(ctx, blocks["conv_b"], zq), x);
            return ggml_add(ctx->ggml_ctx, ggml_mul(ctx->ggml_ctx, apply(ctx, blocks["norm"], x), y), b);
        }
    };

    class ResidualBlock : public GGMLBlock {
        bool shortcut;

    public:
        ResidualBlock(int in, int out, int zq_channels)
            : shortcut(in != out) {
            blocks["norm1"] = std::make_shared<ModulatedNorm>(in, zq_channels);
            blocks["conv1"] = std::make_shared<ReplicateConv>(in, out, 3);
            blocks["norm2"] = std::make_shared<ModulatedNorm>(out, zq_channels);
            blocks["conv2"] = std::make_shared<ReplicateConv>(out, out, 3);
            if (shortcut)
                blocks["shortcut"] = plain_conv(in, out, {1, 1, 1});
        }

        ggml_tensor* forward(GGMLRunnerContext* ctx, ggml_tensor* x, ggml_tensor* zq) {
            auto c    = ctx->ggml_ctx;
            auto norm = [&](const char* name, ggml_tensor* h) { return std::dynamic_pointer_cast<ModulatedNorm>(blocks[name])->forward(ctx, h, zq); };
            auto h    = apply(ctx, blocks["conv1"], ggml_silu(c, norm("norm1", x)));
            h         = apply(ctx, blocks["conv2"], ggml_silu(c, norm("norm2", h)));
            return ggml_add(c, shortcut ? apply(ctx, blocks["shortcut"], x) : x, h);
        }
    };

    // h = linear(h + spatial_conv(h)), optionally after a 2x nearest upsample.
    class SpatialMix : public GGMLBlock {
        bool upsample;

    public:
        SpatialMix(int channels, bool upsample)
            : upsample(upsample) {
            blocks["spatial_conv"] = plain_conv(channels, channels, {1, 3, 3});
            blocks["linear"]       = plain_conv(channels, channels, {1, 1, 1});
        }

        ggml_tensor* forward(GGMLRunnerContext* ctx, ggml_tensor* x) {
            if (upsample)
                x = ggml_upscale(ctx->ggml_ctx, ggml_ext_cont(ctx, x), 2, GGML_SCALE_MODE_NEAREST);
            x = ggml_add(ctx->ggml_ctx, x, apply(ctx, blocks["spatial_conv"], x));
            return apply(ctx, blocks["linear"], x);
        }
    };

    class OutputHead : public GGMLBlock {
    public:
        OutputHead(int in, int out, int zq_channels) {
            blocks["norm"] = std::make_shared<ModulatedNorm>(in, zq_channels);
            blocks["conv"] = std::make_shared<ReplicateConv>(in, out, 3);
        }

        ggml_tensor* forward(GGMLRunnerContext* ctx, ggml_tensor* x, ggml_tensor* zq) {
            x = std::dynamic_pointer_cast<ModulatedNorm>(blocks["norm"])->forward(ctx, x, zq);
            return apply(ctx, blocks["conv"], ggml_silu(ctx->ggml_ctx, x));
        }
    };

    struct UpscalerConfig {
        int in_channels = 64;
        int width_1 = 2048, width_2 = 1024, width_3 = 512;
        int pre_blocks = 5, mid_blocks = 3, post_blocks = 3, adapter_blocks = 2;
        bool has_x2 = false, has_x4 = false;

        static UpscalerConfig detect_from_weights(const String2TensorStorage& weights, const std::string& prefix) {
            UpscalerConfig config;
            auto ne = [&](const std::string& key, int dim) -> int64_t {
                auto it = weights.find(prefix + key);
                return it == weights.end() ? -1 : it->second.ne[dim];
            };
            auto count = [&](const std::string& stem) {
                int n = 0;
                while (weights.count(prefix + stem + std::to_string(n) + ".conv1.weight"))
                    ++n;
                return n;
            };
            config.has_x2 = ne("_models.0.x2_branch.output_proj.conv.weight", 0) > 0;
            config.has_x4 = ne("_models.1.output_proj.conv.weight", 0) > 0 && ne("_models.1.pre_blocks.0.conv1.weight", 0) > 0;
            const std::string ref = config.has_x4 ? "_models.1." : "_models.0.x2_branch.";
            config.in_channels    = int(ne(ref + "output_proj.conv.bias", 0));
            config.width_3        = int(ne(ref + "output_proj.norm.conv_y.bias", 0));
            config.width_2        = int(ne(ref + "mid_blocks.0.conv1.bias", 0));
            config.width_1        = int(ne(ref + "mid_blocks.0.norm1.conv_y.bias", 0));
            config.mid_blocks     = count(ref + "mid_blocks.");
            config.post_blocks    = count(ref + (config.has_x4 ? "post_blocks." : "blocks."));
            config.pre_blocks     = count("_models.1.pre_blocks.");
            config.adapter_blocks = count("_models.0.x2_branch.adapter.");
            if ((!config.has_x2 && !config.has_x4) || config.in_channels <= 0 || config.width_1 <= 0 || config.width_2 <= 0 || config.width_3 <= 0)
                throw std::runtime_error("unsupported Kandinsky 6 VSR latent upscaler weights");
            LOG_INFO("kandinsky6 latent upscaler: x2 %s, x4 %s, widths %d/%d/%d", config.has_x2 ? "yes" : "no",
                     config.has_x4 ? "yes" : "no", config.width_1, config.width_2, config.width_3);
            return config;
        }
    };

    class UpscalerBank : public GGMLBlock {
        UpscalerConfig c;

        void add_stage(const std::string& p, const char* mid, int mid_count, const char* post, int post_count, const char* upsample) {
            std::vector<int> mids(mid_count + 1, c.width_2), posts(post_count + 1, c.width_3);
            mids[0]  = c.width_1;
            posts[0] = c.width_2;
            for (int i = 0; i < mid_count; ++i)
                blocks[p + mid + std::to_string(i)] = std::make_shared<ResidualBlock>(mids[i], mids[i + 1], c.in_channels);
            blocks[p + upsample] = std::make_shared<SpatialMix>(c.width_2, true);
            for (int i = 0; i < post_count; ++i)
                blocks[p + post + std::to_string(i)] = std::make_shared<ResidualBlock>(posts[i], posts[i + 1], c.in_channels);
            blocks[p + "output_proj"] = std::make_shared<OutputHead>(c.width_3, c.in_channels, c.in_channels);
        }

    public:
        UpscalerBank(const UpscalerConfig& config)
            : c(config) {
            if (c.has_x2) {
                const std::string p                 = "_models.0.x2_branch.";
                blocks["_models.0.mid_input_proj.0"] = std::make_shared<ReplicateConv>(c.in_channels, c.width_1, 3);
                for (int i = 0; i < c.adapter_blocks; ++i)
                    blocks[p + "adapter." + std::to_string(i)] = std::make_shared<ResidualBlock>(c.width_1, c.width_1, c.in_channels);
                blocks[p + "finisher"] = std::make_shared<SpatialMix>(c.width_1, false);
                add_stage(p, "mid_blocks.", c.mid_blocks, "blocks.", c.post_blocks, "upsample");
            }
            if (c.has_x4) {
                const std::string p          = "_models.1.";
                blocks[p + "input_proj.0"]   = std::make_shared<ReplicateConv>(c.in_channels, c.width_1, 3);
                for (int i = 0; i < c.pre_blocks; ++i)
                    blocks[p + "pre_blocks." + std::to_string(i)] = std::make_shared<ResidualBlock>(c.width_1, c.width_1, c.in_channels);
                blocks[p + "upsample_1"] = std::make_shared<SpatialMix>(c.width_1, true);
                add_stage(p, "mid_blocks.", c.mid_blocks, "post_blocks.", c.post_blocks, "upsample_2");
            }
        }

        // latents [w, h, t, 64] -> [w*scale, h*scale, t, 64]
        ggml_tensor* forward(GGMLRunnerContext* ctx, ggml_tensor* zq, int scale) {
            auto res  = [&](const std::string& name, ggml_tensor* h) { return std::dynamic_pointer_cast<ResidualBlock>(blocks[name])->forward(ctx, h, zq); };
            auto mix  = [&](const std::string& name, ggml_tensor* h) { return std::dynamic_pointer_cast<SpatialMix>(blocks[name])->forward(ctx, h); };
            auto cut  = [&](ggml_tensor* h, const std::string& name) { sd::ggml_graph_cut::mark_graph_cut(h, "k6_latent_upscaler." + name, "h"); };
            auto tail = [&](const std::string& p, const char* mid, const char* post, const char* upsample, ggml_tensor* h) {
                for (int i = 0; i < c.mid_blocks; ++i) {
                    h = res(p + mid + std::to_string(i), h);
                    cut(h, p + mid + std::to_string(i));
                }
                h = mix(p + upsample, h);
                for (int i = 0; i < c.post_blocks; ++i) {
                    h = res(p + post + std::to_string(i), h);
                    cut(h, p + post + std::to_string(i));
                }
                return std::dynamic_pointer_cast<OutputHead>(blocks[p + "output_proj"])->forward(ctx, h, zq);
            };
            if (scale == 2) {
                const std::string p = "_models.0.x2_branch.";
                auto h              = apply(ctx, blocks["_models.0.mid_input_proj.0"], zq);
                for (int i = 0; i < c.adapter_blocks; ++i)
                    h = res(p + "adapter." + std::to_string(i), h);
                h = mix(p + "finisher", h);
                return tail(p, "mid_blocks.", "blocks.", "upsample", h);
            }
            const std::string p = "_models.1.";
            auto h              = apply(ctx, blocks[p + "input_proj.0"], zq);
            for (int i = 0; i < c.pre_blocks; ++i) {
                h = res(p + "pre_blocks." + std::to_string(i), h);
                cut(h, p + "pre_blocks." + std::to_string(i));
            }
            h = mix(p + "upsample_1", h);
            return tail(p, "mid_blocks.", "post_blocks.", "upsample_2", h);
        }
    };

    struct UpscalerRunner : public GGMLRunner {
        UpscalerConfig config;
        std::unique_ptr<UpscalerBank> model;
        std::string weight_prefix;

        UpscalerRunner(ggml_backend_t backend, const String2TensorStorage& weights, const std::string& prefix,
                       std::shared_ptr<RunnerWeightManager> weight_manager, int only_scale = 0)
            : GGMLRunner(backend, weight_manager),
              config(UpscalerConfig::detect_from_weights(weights, prefix + ".")),
              weight_prefix(prefix) {
            if (only_scale == 2)
                config.has_x4 = false;
            if (only_scale == 4)
                config.has_x2 = false;
            if (!config.has_x2 && !config.has_x4)
                throw std::runtime_error("the latent upscaler file has no " + std::to_string(only_scale) + "x model");
            model = std::make_unique<UpscalerBank>(config);
            model->init(params_ctx, weights, prefix);
        }

        std::string get_desc() override { return "kandinsky6_latent_upscaler"; }
        void get_param_tensors(std::map<std::string, ggml_tensor*>& tensors) { model->get_param_tensors(tensors, weight_prefix); }

        sd::Tensor<float> compute(int n_threads, const sd::Tensor<float>& latent, int scale) {
            if ((scale == 2 && !config.has_x2) || (scale == 4 && !config.has_x4) || (scale != 2 && scale != 4))
                throw std::runtime_error("the loaded Kandinsky 6 latent upscaler has no " + std::to_string(scale) + "x model");
            auto input = latent.dim() == 5 ? latent.squeeze(4) : latent;
            auto graph = [&]() {
                auto gf  = new_graph_custom(MAX_GRAPH_SIZE * 4);
                auto ctx = get_context(gf);
                ggml_build_forward_expand(gf, model->forward(&ctx, make_input(input), scale));
                return gf;
            };
            auto out = restore_trailing_singleton_dims(GGMLRunner::compute(graph, n_threads), 4);
            if (out.empty() || latent.dim() != 5)
                return out;
            return out.unsqueeze(4);
        }
    };
}  // namespace Kandinsky6Upscaler

#endif  // __SD_MODEL_UPSCALER_KANDINSKY6_LATENT_UPSCALER_HPP__
