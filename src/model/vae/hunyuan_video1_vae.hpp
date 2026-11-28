#ifndef __SD_MODEL_VAE_HUNYUAN_VIDEO1_VAE_HPP__
#define __SD_MODEL_VAE_HUNYUAN_VIDEO1_VAE_HPP__

#include <array>
#include <map>
#include <memory>
#include <string>
#include <tuple>
#include <vector>

#include "model/vae/hunyuan_vae.hpp"

// Decoder of the original HunyuanVideo VAE (AutoencoderKLHunyuanVideo): 16 latent channels,
// 8x spatial and 4x temporal compression. Used by Kandinsky 6. Unlike the HunyuanVideo 1.5
// VAE it uses GroupNorm over (channels, frames, height, width), a frame-causal mid-block
// attention, and nearest-neighbour upsampling followed by a causal convolution.
namespace HunyuanVideo1VAE {
    constexpr int GRAPH_SIZE                  = 32768;
    constexpr int GRAPH_SIZE_PER_LATENT_FRAME = 1024;
    constexpr float SCALING_FACTOR            = 0.476986f;
    constexpr int NORM_GROUPS                 = 32;
    constexpr float NORM_EPS                  = 1e-6f;

    // Causal 3D convolution with replicate padding: k-1 copies of the first frame before the clip
    // and k/2 replicated pixels on each spatial side. Pads with one concat per side so whole clips
    // stay cheap (no per-frame slicing).
    class CausalConv3d : public GGMLBlock {
        std::tuple<int, int, int> kernel_size;

        static ggml_tensor* replicate(GGMLRunnerContext* ctx, ggml_tensor* x, int dim, int count, bool front) {
            if (count <= 0) {
                return x;
            }
            const int64_t n = x->ne[dim];
            auto edge       = ggml_ext_slice(ctx, x, dim, front ? 0 : n - 1, front ? 1 : n);
            auto shape      = std::array<int64_t, 4>{x->ne[0], x->ne[1], x->ne[2], x->ne[3]};
            shape[dim]      = count;
            auto pad        = count == 1 ? edge : ggml_repeat(ctx->ggml_ctx, edge, ggml_new_tensor_4d(ctx->ggml_ctx, x->type, shape[0], shape[1], shape[2], shape[3]));
            return front ? ggml_concat(ctx->ggml_ctx, pad, x, dim) : ggml_concat(ctx->ggml_ctx, x, pad, dim);
        }

    public:
        CausalConv3d(int64_t in_channels, int64_t out_channels, std::tuple<int, int, int> kernel_size)
            : kernel_size(kernel_size) {
            blocks["conv"] = std::make_shared<Conv3d>(in_channels, out_channels, kernel_size);
        }

        // x: [W, H, T, C]
        ggml_tensor* forward(GGMLRunnerContext* ctx, ggml_tensor* x) {
            const int pad_t = std::get<0>(kernel_size) - 1;
            const int pad_h = std::get<1>(kernel_size) / 2;
            const int pad_w = std::get<2>(kernel_size) / 2;
            x               = replicate(ctx, x, 2, pad_t, true);
            x               = replicate(ctx, x, 1, pad_h, true);
            x               = replicate(ctx, x, 1, pad_h, false);
            x               = replicate(ctx, x, 0, pad_w, true);
            x               = replicate(ctx, x, 0, pad_w, false);
            return std::dynamic_pointer_cast<Conv3d>(blocks["conv"])->forward(ctx, x);
        }
    };

    // GroupNorm over (C / groups, T, H, W) of a [W, H, T, C] tensor.
    class VideoGroupNorm : public UnaryBlock {
    protected:
        int64_t channels;

        void init_params(ggml_context* ctx, const String2TensorStorage& tensor_storage_map = {}, const std::string prefix = "") override {
            params["weight"] = ggml_new_tensor_1d(ctx, GGML_TYPE_F32, channels);
            params["bias"]   = ggml_new_tensor_1d(ctx, GGML_TYPE_F32, channels);
        }

    public:
        explicit VideoGroupNorm(int64_t channels)
            : channels(channels) {}

        ggml_tensor* forward(GGMLRunnerContext* ctx, ggml_tensor* x) override {
            auto c          = ctx->ggml_ctx;
            const int64_t W = x->ne[0], H = x->ne[1], T = x->ne[2], C = x->ne[3];
            auto y          = ggml_reshape_4d(c, ggml_cont(c, x), W * H, T, C, 1);
            y               = ggml_group_norm(c, y, NORM_GROUPS, NORM_EPS);
            y               = ggml_reshape_4d(c, y, W, H, T, C);
            y               = ggml_mul(c, y, ggml_reshape_4d(c, params["weight"], 1, 1, 1, C));
            return ggml_add(c, y, ggml_reshape_4d(c, params["bias"], 1, 1, 1, C));
        }
    };

    class ResnetBlock : public UnaryBlock {
        int64_t in_channels;
        int64_t out_channels;

    public:
        ResnetBlock(int64_t in_channels, int64_t out_channels)
            : in_channels(in_channels), out_channels(out_channels) {
            blocks["norm1"] = std::make_shared<VideoGroupNorm>(in_channels);
            blocks["conv1"] = std::make_shared<CausalConv3d>(in_channels, out_channels, std::tuple{3, 3, 3});
            blocks["norm2"] = std::make_shared<VideoGroupNorm>(out_channels);
            blocks["conv2"] = std::make_shared<CausalConv3d>(out_channels, out_channels, std::tuple{3, 3, 3});
            if (in_channels != out_channels) {
                blocks["conv_shortcut"] = std::make_shared<CausalConv3d>(in_channels, out_channels, std::tuple{1, 1, 1});
            }
        }

        ggml_tensor* forward(GGMLRunnerContext* ctx, ggml_tensor* x) override {
            auto norm1 = std::dynamic_pointer_cast<VideoGroupNorm>(blocks["norm1"]);
            auto conv1 = std::dynamic_pointer_cast<CausalConv3d>(blocks["conv1"]);
            auto norm2 = std::dynamic_pointer_cast<VideoGroupNorm>(blocks["norm2"]);
            auto conv2 = std::dynamic_pointer_cast<CausalConv3d>(blocks["conv2"]);

            auto h = conv1->forward(ctx, ggml_silu(ctx->ggml_ctx, norm1->forward(ctx, x)));
            h      = conv2->forward(ctx, ggml_silu(ctx->ggml_ctx, norm2->forward(ctx, h)));
            if (in_channels != out_channels) {
                x = std::dynamic_pointer_cast<CausalConv3d>(blocks["conv_shortcut"])->forward(ctx, x);
            }
            return ggml_add(ctx->ggml_ctx, h, x);
        }
    };

    // Single-head self-attention over all tokens; a token sees every token of its own and earlier
    // frames (diffusers' prepare_causal_attention_mask). Evaluated frame by frame so no mask is needed.
    class MidAttention : public UnaryBlock {
        int64_t channels;

    public:
        explicit MidAttention(int64_t channels)
            : channels(channels) {
            blocks["group_norm"] = std::make_shared<VideoGroupNorm>(channels);
            blocks["to_q"]       = std::make_shared<Linear>(channels, channels);
            blocks["to_k"]       = std::make_shared<Linear>(channels, channels);
            blocks["to_v"]       = std::make_shared<Linear>(channels, channels);
            blocks["to_out.0"]   = std::make_shared<Linear>(channels, channels);
        }

        ggml_tensor* forward(GGMLRunnerContext* ctx, ggml_tensor* x) override {
            auto c          = ctx->ggml_ctx;
            const int64_t W = x->ne[0], H = x->ne[1], T = x->ne[2], C = x->ne[3];
            const int64_t L = W * H;

            auto h = std::dynamic_pointer_cast<VideoGroupNorm>(blocks["group_norm"])->forward(ctx, x);
            h      = ggml_cont(c, ggml_transpose(c, ggml_reshape_2d(c, h, L * T, C)));  // [C, tokens]
            auto q = std::dynamic_pointer_cast<Linear>(blocks["to_q"])->forward(ctx, h);
            auto k = std::dynamic_pointer_cast<Linear>(blocks["to_k"])->forward(ctx, h);
            auto v = std::dynamic_pointer_cast<Linear>(blocks["to_v"])->forward(ctx, h);

            ggml_tensor* out = nullptr;
            for (int64_t t = 0; t < T; ++t) {
                auto q_t  = ggml_view_3d(c, q, C, L, 1, q->nb[1], q->nb[1] * L, q->nb[1] * L * t);
                auto k_t  = ggml_view_3d(c, k, C, L * (t + 1), 1, k->nb[1], k->nb[1] * L * (t + 1), 0);
                auto v_t  = ggml_view_3d(c, v, C, L * (t + 1), 1, v->nb[1], v->nb[1] * L * (t + 1), 0);
                auto attn = ggml_ext_attention_ext(ctx, ggml_cont(c, q_t), ggml_cont(c, k_t), ggml_cont(c, v_t), 1,
                                                   nullptr, false, ctx->flash_attn_enabled);
                out       = out == nullptr ? attn : ggml_concat(c, out, attn, 1);
            }
            out = std::dynamic_pointer_cast<Linear>(blocks["to_out.0"])->forward(ctx, ggml_reshape_2d(c, out, C, L * T));
            out = ggml_reshape_4d(c, ggml_cont(c, ggml_transpose(c, out)), W, H, T, C);
            return ggml_add(c, out, x);
        }
    };

    class Upsample : public UnaryBlock {
        bool temporal;

    public:
        Upsample(int64_t channels, bool temporal)
            : temporal(temporal) {
            blocks["conv"] = std::make_shared<CausalConv3d>(channels, channels, std::tuple{3, 3, 3});
        }

        // Nearest-neighbour x2 in height and width; with temporal upsampling every frame but the
        // first is repeated, so T latent frames become 2T - 1.
        ggml_tensor* forward(GGMLRunnerContext* ctx, ggml_tensor* x) override {
            auto c = ctx->ggml_ctx;
            if (temporal && x->ne[2] > 1) {
                const int64_t W = x->ne[0], H = x->ne[1], T = x->ne[2], C = x->ne[3];
                auto first      = ggml_ext_slice(ctx, x, 2, 0, 1);
                auto rest       = ggml_reshape_4d(c, ggml_ext_slice(ctx, x, 2, 1, T), W * H, 1, T - 1, C);
                rest            = ggml_repeat(c, rest, ggml_new_tensor_4d(c, rest->type, W * H, 2, T - 1, C));
                rest            = ggml_reshape_4d(c, rest, W, H, 2 * (T - 1), C);
                x               = ggml_concat(c, first, rest, 2);
            }
            x = ggml_upscale(c, x, 2, GGML_SCALE_MODE_NEAREST);
            return std::dynamic_pointer_cast<CausalConv3d>(blocks["conv"])->forward(ctx, x);
        }
    };

    class UpBlock : public UnaryBlock {
        int num_layers;
        bool upsample;

    public:
        UpBlock(int64_t in_channels, int64_t out_channels, int num_layers, bool upsample, bool temporal)
            : num_layers(num_layers), upsample(upsample) {
            for (int i = 0; i < num_layers; ++i) {
                blocks["resnets." + std::to_string(i)] = std::make_shared<ResnetBlock>(i == 0 ? in_channels : out_channels, out_channels);
            }
            if (upsample) {
                blocks["upsamplers.0"] = std::make_shared<Upsample>(out_channels, temporal);
            }
        }

        ggml_tensor* forward(GGMLRunnerContext* ctx, ggml_tensor* x) override {
            for (int i = 0; i < num_layers; ++i) {
                x = std::dynamic_pointer_cast<ResnetBlock>(blocks["resnets." + std::to_string(i)])->forward(ctx, x);
            }
            if (upsample) {
                x = std::dynamic_pointer_cast<Upsample>(blocks["upsamplers.0"])->forward(ctx, x);
            }
            return x;
        }
    };

    class Decoder : public GGMLBlock {
        std::vector<int64_t> channels{512, 512, 256, 128};

    public:
        Decoder(int64_t latent_channels = 16, int64_t out_channels = 3, int layers_per_block = 2) {
            blocks["conv_in"]                = std::make_shared<CausalConv3d>(latent_channels, channels[0], std::tuple{3, 3, 3});
            blocks["mid_block.resnets.0"]    = std::make_shared<ResnetBlock>(channels[0], channels[0]);
            blocks["mid_block.attentions.0"] = std::make_shared<MidAttention>(channels[0]);
            blocks["mid_block.resnets.1"]    = std::make_shared<ResnetBlock>(channels[0], channels[0]);
            int64_t in                       = channels[0];
            for (int i = 0; i < 4; ++i) {
                // 3 spatial and 2 temporal doublings: blocks 0-2 upsample space, blocks 1-2 also time.
                blocks["up_blocks." + std::to_string(i)] = std::make_shared<UpBlock>(in, channels[i], layers_per_block + 1, i < 3, i == 1 || i == 2);
                in                                       = channels[i];
            }
            blocks["conv_norm_out"] = std::make_shared<VideoGroupNorm>(channels.back());
            blocks["conv_out"]      = std::make_shared<CausalConv3d>(channels.back(), out_channels, std::tuple{3, 3, 3});
        }

        ggml_tensor* forward(GGMLRunnerContext* ctx, ggml_tensor* z) {
            auto block = [&](const std::string& name) { return std::dynamic_pointer_cast<UnaryBlock>(blocks[name]); };
            auto h     = std::dynamic_pointer_cast<CausalConv3d>(blocks["conv_in"])->forward(ctx, z);
            h          = block("mid_block.resnets.0")->forward(ctx, h);
            h          = block("mid_block.attentions.0")->forward(ctx, h);
            h          = block("mid_block.resnets.1")->forward(ctx, h);
            for (int i = 0; i < 4; ++i) {
                h = block("up_blocks." + std::to_string(i))->forward(ctx, h);
            }
            h = ggml_silu(ctx->ggml_ctx, block("conv_norm_out")->forward(ctx, h));
            return std::dynamic_pointer_cast<CausalConv3d>(blocks["conv_out"])->forward(ctx, h);
        }
    };

    class HunyuanVideo1VAERunner : public VAE {
    protected:
        Conv3d post_quant_conv;
        Decoder decoder;

    public:
        HunyuanVideo1VAERunner(ggml_backend_t backend,
                               const String2TensorStorage& tensor_storage_map,
                               const std::string& prefix,
                               SDVersion version,
                               std::shared_ptr<RunnerWeightManager> weight_manager = nullptr)
            : VAE(version, backend, prefix, weight_manager),
              post_quant_conv(16, 16, std::tuple{1, 1, 1}) {
            post_quant_conv.init(params_ctx, tensor_storage_map, prefix + ".post_quant_conv");
            decoder.init(params_ctx, tensor_storage_map, prefix + ".decoder");
        }

        std::string get_desc() override {
            return "hunyuan_video1_vae";
        }

        bool supports_temporal_tiling(VAETemporalDirection direction) const override {
            return direction == VAETemporalDirection::DECODE;
        }

        int get_temporal_tile_output_scale(VAETemporalDirection direction) const override {
            SD_UNUSED(direction);
            return 4;
        }

        // Causal decoding restarts at every tile; two latent frames of overlap keep the restart
        // out of the blended frames (measured 42 dB vs an untiled decode; overlap 3 gives 44 dB).
        int get_default_temporal_tile_overlap(VAETemporalDirection direction) const override {
            SD_UNUSED(direction);
            return 2;
        }

        void get_param_tensors(std::map<std::string, ggml_tensor*>& tensors) override {
            post_quant_conv.get_param_tensors(tensors, weight_prefix + ".post_quant_conv");
            decoder.get_param_tensors(tensors, weight_prefix + ".decoder");
        }

        int get_encoder_output_channels(int input_channels) override {
            SD_UNUSED(input_channels);
            return 32;
        }

        sd::Tensor<float> vae_output_to_latents(const sd::Tensor<float>& vae_output,
                                                std::shared_ptr<RNG> rng,
                                                VAEPosterior,
                                                bool) override {
            SD_UNUSED(rng);
            return vae_output;
        }

        sd::Tensor<float> diffusion_to_vae_latents(const sd::Tensor<float>& latents) override {
            return latents / SCALING_FACTOR;
        }

        sd::Tensor<float> vae_to_diffusion_latents(const sd::Tensor<float>& latents) override {
            return latents * SCALING_FACTOR;
        }

        ggml_cgraph* build_graph(const sd::Tensor<float>& input_tensor) {
            const size_t graph_size = std::max<size_t>(GRAPH_SIZE, GRAPH_SIZE_PER_LATENT_FRAME * static_cast<size_t>(input_tensor.shape()[2]));
            ggml_cgraph* gf         = new_graph_custom(graph_size);
            ggml_tensor* z          = make_input(input_tensor);
            auto runner_ctx         = get_context();
            z                       = post_quant_conv.forward(&runner_ctx, z);
            ggml_build_forward_expand(gf, decoder.forward(&runner_ctx, z));
            return gf;
        }

        sd::Tensor<float> _compute(const int n_threads,
                                   const sd::Tensor<float>& input,
                                   bool decode_graph) override {
            if (!decode_graph) {
                LOG_ERROR("the HunyuanVideo VAE encoder is not supported");
                return {};
            }
            sd::Tensor<float> expanded;
            if (input.dim() == 4) {
                expanded = input.unsqueeze(2);
            }
            const auto& graph_input = expanded.empty() ? input : expanded;
            auto get_graph          = [&]() -> ggml_cgraph* { return build_graph(graph_input); };
            auto output             = restore_trailing_singleton_dims(GGMLRunner::compute(get_graph, n_threads, false), graph_input.dim());
            if (!output.empty() && input.dim() == 4) {
                output.squeeze_(2);
            }
            return output;
        }
    };
}  // namespace HunyuanVideo1VAE

#endif  // __SD_MODEL_VAE_HUNYUAN_VIDEO1_VAE_HPP__
