#ifndef __SD_MODEL_DIFFUSION_KANDINSKY6_HPP__
#define __SD_MODEL_DIFFUSION_KANDINSKY6_HPP__

#include <array>
#include <cinttypes>
#include <cmath>
#include <map>
#include <memory>
#include <utility>
#include "core/ggml_extend_backend.h"
#include "core/ggml_tensor_utils.h"
#include "model/common/block.hpp"
#include "model/common/rope.hpp"
#include "model/diffusion/model.hpp"

// Kandinsky 6.0 multimodal transformer (Kandinsky6Transformer3DModel in diffusers).
// Video and audio latents are denoised together by fused blocks with video<->audio
// cross attention; each modality has its own text branch fed by Qwen2.5-VL tokens
// and a CLIP-L pooled vector. Distilled checkpoints predict n_grid values per channel
// for the PiFlow sampler.
namespace Kandinsky6 {
    constexpr int KANDINSKY6_GRAPH_SIZE = 32768;

    struct Kandinsky6Config {
        int64_t in_visual_dim      = 16;
        int64_t out_visual_dim     = 160;
        int64_t in_text_dim        = 3584;
        int64_t in_text_dim2       = 768;
        int64_t time_dim           = 512;
        int64_t model_dim          = 1792;
        int64_t ff_dim             = 7168;
        int num_text_blocks        = 2;
        int num_visual_blocks      = 32;
        int64_t in_audio_dim       = 40;
        int64_t out_audio_dim      = 400;
        int64_t model_dim_a        = 896;
        int64_t time_dim_a         = 512;
        int64_t ff_dim_a           = 3584;
        int64_t head_dim           = 64;
        std::array<int, 3> axes_dims{16, 24, 24};
        std::array<int, 3> patch_size{1, 2, 2};
        std::array<float, 3> rope_scale{1.f, 2.f, 2.f};
        float audio_freqs_scaling  = 0.144f;
        float rope_theta           = 10000.f;
        bool visual_cond           = true;
        int64_t visual_token_types = 0;
        bool cross_gates           = true;
        // PyTorch nn.RMSNorm(eps=None) uses the input dtype's epsilon: 2^-23 for fp32.
        float qk_norm_eps = 1.1920929e-07f;
        float norm_eps    = 1e-5f;
        // PiFlow policy (PiflowScheduler config of the distilled checkpoints).
        float piflow_shift       = 5.f;
        int piflow_substeps      = 128;
        float piflow_eps         = 1e-6f;

        int64_t visual_in_channels() const { return visual_cond ? 2 * in_visual_dim + 1 : in_visual_dim; }
        int64_t n_grid() const { return out_visual_dim / in_visual_dim; }
        // Distilled checkpoints predict n_grid PiFlow velocities per channel; the others one flow velocity.
        bool is_piflow() const { return n_grid() > 1; }

        static Kandinsky6Config detect_from_weights(const String2TensorStorage& tensor_storage_map,
                                                    const std::string& prefix) {
            Kandinsky6Config config;
            const std::string p = prefix.empty() ? "" : prefix + ".";
            auto ne             = [&](const std::string& name, int dim) -> int64_t {
                auto it = tensor_storage_map.find(p + name);
                return it == tensor_storage_map.end() ? -1 : it->second.ne[dim];
            };
            int visual_blocks = 0, text_blocks = 0;
            for (const auto& [name, _] : tensor_storage_map) {
                if (!starts_with(name, p)) {
                    continue;
                }
                const std::string rest = name.substr(p.size());
                auto count_block       = [&](const std::string& stem, int& count) {
                    if (!starts_with(rest, stem)) {
                        return;
                    }
                    int index = atoi(rest.c_str() + stem.size());
                    count     = std::max(count, index + 1);
                };
                count_block("visual_transformer_blocks.", visual_blocks);
                count_block("video_text_transformer_blocks.", text_blocks);
            }
            config.num_visual_blocks = visual_blocks;
            config.num_text_blocks   = text_blocks;

            const int64_t patch_volume = config.patch_size[0] * config.patch_size[1] * config.patch_size[2];
            config.model_dim           = ne("visual_embeddings.in_layer.weight", 1);
            const int64_t vis_in       = ne("visual_embeddings.in_layer.weight", 0) / patch_volume;
            config.out_visual_dim      = ne("out_layer.out_layer.weight", 1) / patch_volume;
            config.ff_dim              = ne("visual_transformer_blocks.0.video_dec_block.feed_forward.net.0.proj.weight", 1);
            config.in_text_dim         = ne("video_text_embeddings.in_layer.weight", 0);
            config.in_text_dim2        = ne("video_pooled_text_embeddings.in_layer.weight", 0);
            config.time_dim            = ne("video_time_embeddings.timestep_embedder.linear_2.weight", 1);
            config.model_dim_a         = ne("audio_embeddings.in_layer.weight", 1);
            config.in_audio_dim        = ne("audio_embeddings.in_layer.weight", 0);
            config.out_audio_dim       = ne("audio_out_layer.out_layer.weight", 1);
            config.time_dim_a          = ne("audio_time_embeddings.timestep_embedder.linear_2.weight", 1);
            config.ff_dim_a            = ne("visual_transformer_blocks.0.audio_dec_block.feed_forward.net.0.proj.weight", 1);
            config.head_dim            = ne("visual_transformer_blocks.0.video_dec_block.self_attention.query_norm.weight", 0);
            const int64_t head_dim_a   = ne("visual_transformer_blocks.0.audio_dec_block.self_attention.query_norm.weight", 0);
            config.visual_token_types  = std::max<int64_t>(0, ne("visual_token_type_embeddings.weight", 1));
            config.cross_gates         = ne("visual_transformer_blocks.0.va_modulation.out_layer.weight", 1) ==
                                 2 * config.model_dim + config.model_dim_a;

            // The video<->audio cross attention rotates audio keys with video RoPE (ca_rope), so both
            // modalities must share one head size. Released checkpoints split it 1/4 : 3/8 : 3/8 over
            // (t, h, w): Lite 64 -> {16, 24, 24}, Pro 128 -> {32, 48, 48}.
            if (config.model_dim <= 0 || config.num_visual_blocks <= 0 || config.model_dim_a <= 0 ||
                config.head_dim <= 0 || config.head_dim % 16 != 0 || head_dim_a != config.head_dim ||
                config.model_dim % config.head_dim != 0 || config.model_dim_a % config.head_dim != 0) {
                throw std::runtime_error("unsupported Kandinsky 6 transformer weights");
            }
            config.axes_dims = {static_cast<int>(config.head_dim / 4), static_cast<int>(config.head_dim * 3 / 8),
                                static_cast<int>(config.head_dim * 3 / 8)};
            config.in_visual_dim = config.out_visual_dim;
            for (int64_t channels = 1; channels <= vis_in; ++channels) {
                if (2 * channels + 1 == vis_in && config.out_visual_dim % channels == 0) {
                    config.in_visual_dim = channels;
                    config.visual_cond   = true;
                    break;
                }
            }
            if (!config.visual_cond || !config.cross_gates) {
                throw std::runtime_error("only Kandinsky 6 checkpoints with visual conditioning and cross gates are supported");
            }
            if (config.out_audio_dim != config.n_grid() * config.in_audio_dim) {
                throw std::runtime_error("Kandinsky 6 video and audio heads predict different grid sizes");
            }

            LOG_INFO("kandinsky6: model_dim = %" PRId64 ", head_dim = %" PRId64 ", visual_blocks = %d, text_blocks = %d, %s",
                     config.model_dim, config.head_dim, config.num_visual_blocks, config.num_text_blocks,
                     config.is_piflow() ? "PiFlow (distilled) output" : "flow velocity output");
            return config;
        }
    };

    // [N, dim] modulation chunk -> [N, 1, dim] so it broadcasts over tokens.
    static ggml_tensor* param_chunk(GGMLRunnerContext* ctx, ggml_tensor* params, int64_t offset, int64_t size) {
        auto view = ggml_view_2d(ctx->ggml_ctx, params, size, params->ne[1], params->nb[1], offset * ggml_element_size(params));
        view      = ggml_cont(ctx->ggml_ctx, view);
        return ggml_reshape_3d(ctx->ggml_ctx, view, size, 1, params->ne[1]);
    }

    // normed * (1 + scale) + shift
    static ggml_tensor* scale_shift(GGMLRunnerContext* ctx, ggml_tensor* normed, ggml_tensor* scale, ggml_tensor* shift) {
        return ggml_ext_modulate(ctx, normed, shift, scale);
    }

    static ggml_tensor* gate_sum(GGMLRunnerContext* ctx, ggml_tensor* x, ggml_tensor* out, ggml_tensor* gate) {
        return ggml_add(ctx->ggml_ctx, x, ggml_mul(ctx->ggml_ctx, out, gate));
    }

    static ggml_tensor* plain_norm(GGMLRunnerContext* ctx, ggml_tensor* x, float eps) {
        return ggml_ext_layer_norm(ctx, x, nullptr, nullptr, eps);
    }

    // The reference keeps the time embeddings and modulations in fp32 (_keep_in_fp32_modules).
    // Lite stores these weights as F32; Pro stores them as bf16 (9.4 GB). Converting them to F32 at
    // load would double that and stop them from being memory-mapped, so they keep their stored type
    // and are cast to F32 in the graph, which keeps the activations in fp32 as well.
    class F32Linear : public Linear {
    public:
        F32Linear(int64_t in_features, int64_t out_features)
            : Linear(in_features, out_features, true) {}

        ggml_tensor* forward(GGMLRunnerContext* ctx, ggml_tensor* x) override {
            ggml_tensor* w = params["weight"];
            if (w->type == GGML_TYPE_F32) {
                return Linear::forward(ctx, x);
            }
            w = ggml_cast(ctx->ggml_ctx, w, GGML_TYPE_F32);
            return ggml_add(ctx->ggml_ctx, ggml_mul_mat(ctx->ggml_ctx, w, x), params["bias"]);
        }
    };

    class Modulation : public GGMLBlock {
    public:
        Modulation(int64_t time_dim, int64_t out_dim) {
            blocks["out_layer"] = std::make_shared<F32Linear>(time_dim, out_dim);
        }

        ggml_tensor* forward(GGMLRunnerContext* ctx, ggml_tensor* t) {
            auto out_layer = std::dynamic_pointer_cast<F32Linear>(blocks["out_layer"]);
            return out_layer->forward(ctx, ggml_silu(ctx->ggml_ctx, t));
        }
    };

    class TimeEmbeddings : public GGMLBlock {
        int64_t model_dim;

    public:
        TimeEmbeddings(int64_t model_dim, int64_t time_dim)
            : model_dim(model_dim) {
            blocks["timestep_embedder.linear_1"] = std::make_shared<F32Linear>(model_dim, time_dim);
            blocks["timestep_embedder.linear_2"] = std::make_shared<F32Linear>(time_dim, time_dim);
        }

        ggml_tensor* forward(GGMLRunnerContext* ctx, ggml_tensor* timesteps) {
            auto linear_1 = std::dynamic_pointer_cast<F32Linear>(blocks["timestep_embedder.linear_1"]);
            auto linear_2 = std::dynamic_pointer_cast<F32Linear>(blocks["timestep_embedder.linear_2"]);
            auto x        = ggml_ext_timestep_embedding(ctx, timesteps, static_cast<int>(model_dim));
            x             = ggml_silu(ctx->ggml_ctx, linear_1->forward(ctx, x));
            return linear_2->forward(ctx, x);
        }
    };

    class TextEmbeddings : public GGMLBlock {
    public:
        TextEmbeddings(int64_t in_dim, int64_t out_dim, float eps) {
            blocks["in_layer"] = std::make_shared<Linear>(in_dim, out_dim);
            blocks["norm"]     = std::make_shared<LayerNorm>(out_dim, eps);
        }

        ggml_tensor* forward(GGMLRunnerContext* ctx, ggml_tensor* x) {
            auto in_layer = std::dynamic_pointer_cast<Linear>(blocks["in_layer"]);
            auto norm     = std::dynamic_pointer_cast<LayerNorm>(blocks["norm"]);
            return norm->forward(ctx, in_layer->forward(ctx, x));
        }
    };

    class FeedForward : public GGMLBlock {
    public:
        FeedForward(int64_t dim, int64_t inner_dim) {
            blocks["net.0.proj"] = std::make_shared<Linear>(dim, inner_dim, false);
            blocks["net.2"]      = std::make_shared<Linear>(inner_dim, dim, false);
        }

        ggml_tensor* forward(GGMLRunnerContext* ctx, ggml_tensor* x) {
            auto proj = std::dynamic_pointer_cast<Linear>(blocks["net.0.proj"]);
            auto out  = std::dynamic_pointer_cast<Linear>(blocks["net.2"]);
            x         = proj->forward(ctx, x);
            x         = ggml_gelu_erf(ctx->ggml_ctx, x);
            return out->forward(ctx, x);
        }
    };

    class Attention : public GGMLBlock {
        int64_t num_heads;
        int64_t head_dim;

    public:
        Attention(int64_t dim, int64_t head_dim, int64_t kv_dim, float qk_eps)
            : num_heads(dim / head_dim), head_dim(head_dim) {
            blocks["to_query"]   = std::make_shared<Linear>(dim, dim);
            blocks["to_key"]     = std::make_shared<Linear>(kv_dim, dim);
            blocks["to_value"]   = std::make_shared<Linear>(kv_dim, dim);
            blocks["query_norm"] = std::make_shared<RMSNorm>(head_dim, qk_eps);
            blocks["key_norm"]   = std::make_shared<RMSNorm>(head_dim, qk_eps);
            blocks["out_layer"]  = std::make_shared<Linear>(dim, dim);
        }

        // x: [N, Lq, dim]; kv: [N, Lk, kv_dim] (x when null); pe_q/pe_k: [L, head_dim/2, 2, 2] or null
        ggml_tensor* forward(GGMLRunnerContext* ctx,
                             ggml_tensor* x,
                             ggml_tensor* kv   = nullptr,
                             ggml_tensor* pe_q = nullptr,
                             ggml_tensor* pe_k = nullptr) {
            auto to_query   = std::dynamic_pointer_cast<Linear>(blocks["to_query"]);
            auto to_key     = std::dynamic_pointer_cast<Linear>(blocks["to_key"]);
            auto to_value   = std::dynamic_pointer_cast<Linear>(blocks["to_value"]);
            auto query_norm = std::dynamic_pointer_cast<RMSNorm>(blocks["query_norm"]);
            auto key_norm   = std::dynamic_pointer_cast<RMSNorm>(blocks["key_norm"]);
            auto out_layer  = std::dynamic_pointer_cast<Linear>(blocks["out_layer"]);
            if (kv == nullptr) {
                kv = x;
            }
            const int64_t N  = x->ne[2];
            const int64_t Lq = x->ne[1];
            const int64_t Lk = kv->ne[1];

            auto q = ggml_reshape_4d(ctx->ggml_ctx, to_query->forward(ctx, x), head_dim, num_heads, Lq, N);
            auto k = ggml_reshape_4d(ctx->ggml_ctx, to_key->forward(ctx, kv), head_dim, num_heads, Lk, N);
            auto v = ggml_reshape_4d(ctx->ggml_ctx, to_value->forward(ctx, kv), head_dim, num_heads, Lk, N);
            q      = query_norm->forward(ctx, q);
            k      = key_norm->forward(ctx, k);

            ggml_tensor* out;
            if (pe_q != nullptr || pe_k != nullptr) {
                GGML_ASSERT(pe_q != nullptr && pe_k != nullptr);
                q   = Rope::apply_rope(ctx, q, pe_q);
                k   = Rope::apply_rope(ctx, k, pe_k);
                out = ggml_ext_attention_ext(ctx, q, k, v, num_heads, nullptr, true, ctx->flash_attn_enabled);
            } else {
                q   = ggml_reshape_3d(ctx->ggml_ctx, q, head_dim * num_heads, Lq, N);
                k   = ggml_reshape_3d(ctx->ggml_ctx, k, head_dim * num_heads, Lk, N);
                v   = ggml_reshape_3d(ctx->ggml_ctx, v, head_dim * num_heads, Lk, N);
                out = ggml_ext_attention_ext(ctx, q, k, v, num_heads, nullptr, false, ctx->flash_attn_enabled);
            }
            return out_layer->forward(ctx, out);
        }
    };

    class TextEncoderBlock : public GGMLBlock {
        int64_t dim;
        float eps;

    public:
        TextEncoderBlock(int64_t dim, int64_t time_dim, int64_t ff_dim, int64_t head_dim, float qk_eps, float eps)
            : dim(dim), eps(eps) {
            blocks["text_modulation"] = std::make_shared<Modulation>(time_dim, 6 * dim);
            blocks["attn"]            = std::make_shared<Attention>(dim, head_dim, dim, qk_eps);
            blocks["feed_forward"]    = std::make_shared<FeedForward>(dim, ff_dim);
        }

        ggml_tensor* forward(GGMLRunnerContext* ctx, ggml_tensor* x, ggml_tensor* t, ggml_tensor* pe) {
            auto modulation = std::dynamic_pointer_cast<Modulation>(blocks["text_modulation"]);
            auto attn       = std::dynamic_pointer_cast<Attention>(blocks["attn"]);
            auto ff         = std::dynamic_pointer_cast<FeedForward>(blocks["feed_forward"]);
            auto m          = modulation->forward(ctx, t);  // [sa shift, scale, gate | ff shift, scale, gate]

            auto h = scale_shift(ctx, plain_norm(ctx, x, eps), param_chunk(ctx, m, dim, dim), param_chunk(ctx, m, 0, dim));
            x      = gate_sum(ctx, x, attn->forward(ctx, h, nullptr, pe, pe), param_chunk(ctx, m, 2 * dim, dim));
            h      = scale_shift(ctx, plain_norm(ctx, x, eps), param_chunk(ctx, m, 4 * dim, dim), param_chunk(ctx, m, 3 * dim, dim));
            return gate_sum(ctx, x, ff->forward(ctx, h), param_chunk(ctx, m, 5 * dim, dim));
        }
    };

    class DecoderBlock : public GGMLBlock {
    public:
        DecoderBlock(int64_t dim, int64_t time_dim, int64_t ff_dim, int64_t head_dim, float qk_eps) {
            blocks["visual_modulation"] = std::make_shared<Modulation>(time_dim, 9 * dim);
            blocks["self_attention"]    = std::make_shared<Attention>(dim, head_dim, dim, qk_eps);
            blocks["cross_attention"]   = std::make_shared<Attention>(dim, head_dim, dim, qk_eps);
            blocks["feed_forward"]      = std::make_shared<FeedForward>(dim, ff_dim);
        }

        std::shared_ptr<Modulation> modulation() { return std::dynamic_pointer_cast<Modulation>(blocks["visual_modulation"]); }
        std::shared_ptr<Attention> self_attention() { return std::dynamic_pointer_cast<Attention>(blocks["self_attention"]); }
        std::shared_ptr<Attention> cross_attention() { return std::dynamic_pointer_cast<Attention>(blocks["cross_attention"]); }
        std::shared_ptr<FeedForward> feed_forward() { return std::dynamic_pointer_cast<FeedForward>(blocks["feed_forward"]); }
    };

    class FusedBlock : public GGMLBlock {
        int64_t dim;
        int64_t dim_a;
        float eps;

    public:
        FusedBlock(const Kandinsky6Config& config)
            : dim(config.model_dim), dim_a(config.model_dim_a), eps(config.norm_eps) {
            blocks["video_dec_block"]    = std::make_shared<DecoderBlock>(config.model_dim, config.time_dim, config.ff_dim, config.head_dim, config.qk_norm_eps);
            blocks["audio_dec_block"]    = std::make_shared<DecoderBlock>(config.model_dim_a, config.time_dim_a, config.ff_dim_a, config.head_dim, config.qk_norm_eps);
            blocks["va_cross_attention"] = std::make_shared<Attention>(config.model_dim, config.head_dim, config.model_dim_a, config.qk_norm_eps);
            blocks["av_cross_attention"] = std::make_shared<Attention>(config.model_dim_a, config.head_dim, config.model_dim, config.qk_norm_eps);
            blocks["va_modulation"]      = std::make_shared<Modulation>(config.time_dim, 2 * config.model_dim + config.model_dim_a);
            blocks["av_modulation"]      = std::make_shared<Modulation>(config.time_dim_a, 2 * config.model_dim_a + config.model_dim);
        }

        // Released checkpoints: ca_rope, cross_gates and fix_modulation are all enabled.
        std::pair<ggml_tensor*, ggml_tensor*> forward(GGMLRunnerContext* ctx,
                                                      ggml_tensor* vis,
                                                      ggml_tensor* aud,
                                                      ggml_tensor* text_v,
                                                      ggml_tensor* text_a,
                                                      ggml_tensor* t_v,
                                                      ggml_tensor* t_a,
                                                      ggml_tensor* vis_pe,
                                                      ggml_tensor* aud_pe) {
            auto video   = std::dynamic_pointer_cast<DecoderBlock>(blocks["video_dec_block"]);
            auto v_mod   = video->modulation();
            auto v_self  = video->self_attention();
            auto v_cross = video->cross_attention();
            auto v_ff    = video->feed_forward();

            // visual_modulation: [sa | ca | ff] x [shift, scale, gate]
            auto mv = v_mod->forward(ctx, t_v);
            auto h  = scale_shift(ctx, plain_norm(ctx, vis, eps), param_chunk(ctx, mv, dim, dim), param_chunk(ctx, mv, 0, dim));
            vis     = gate_sum(ctx, vis, v_self->forward(ctx, h, nullptr, vis_pe, vis_pe), param_chunk(ctx, mv, 2 * dim, dim));

            auto vis_pre_ca = scale_shift(ctx, plain_norm(ctx, vis, eps), param_chunk(ctx, mv, 4 * dim, dim), param_chunk(ctx, mv, 3 * dim, dim));
            auto vis_out_t  = v_cross->forward(ctx, vis_pre_ca, text_v);
            auto gate_v     = param_chunk(ctx, mv, 5 * dim, dim);

            if (aud != nullptr) {
                auto audio   = std::dynamic_pointer_cast<DecoderBlock>(blocks["audio_dec_block"]);
                auto a_mod   = audio->modulation();
                auto a_self  = audio->self_attention();
                auto a_cross = audio->cross_attention();
                auto a_ff    = audio->feed_forward();
                auto va_mod  = std::dynamic_pointer_cast<Modulation>(blocks["va_modulation"]);
                auto av_mod  = std::dynamic_pointer_cast<Modulation>(blocks["av_modulation"]);
                auto va_attn = std::dynamic_pointer_cast<Attention>(blocks["va_cross_attention"]);
                auto av_attn = std::dynamic_pointer_cast<Attention>(blocks["av_cross_attention"]);

                auto ma = a_mod->forward(ctx, t_a);
                h       = scale_shift(ctx, plain_norm(ctx, aud, eps), param_chunk(ctx, ma, dim_a, dim_a), param_chunk(ctx, ma, 0, dim_a));
                aud     = gate_sum(ctx, aud, a_self->forward(ctx, h, nullptr, aud_pe, aud_pe), param_chunk(ctx, ma, 2 * dim_a, dim_a));

                auto aud_pre_ca = scale_shift(ctx, plain_norm(ctx, aud, eps), param_chunk(ctx, ma, 4 * dim_a, dim_a), param_chunk(ctx, ma, 3 * dim_a, dim_a));
                aud             = gate_sum(ctx, aud, a_cross->forward(ctx, aud_pre_ca, text_a), param_chunk(ctx, ma, 5 * dim_a, dim_a));

                // fix_modulation: va uses the video time embedding, av the audio one.
                // cross_gates: va = [shift(dim), scale(dim), gate(dim_a)], av = [shift(dim_a), scale(dim_a), gate(dim)].
                auto va = va_mod->forward(ctx, t_v);
                auto av = av_mod->forward(ctx, t_a);
                vis     = gate_sum(ctx, vis, vis_out_t, gate_v);

                auto vis_for_va   = scale_shift(ctx, plain_norm(ctx, vis, eps), param_chunk(ctx, va, dim, dim), param_chunk(ctx, va, 0, dim));
                auto aud_for_av   = scale_shift(ctx, plain_norm(ctx, aud, eps), param_chunk(ctx, av, dim_a, dim_a), param_chunk(ctx, av, 0, dim_a));
                auto vis_from_aud = va_attn->forward(ctx, vis_for_va, aud_pre_ca, vis_pe, aud_pe);
                auto aud_from_vis = av_attn->forward(ctx, aud_for_av, vis_pre_ca, aud_pe, vis_pe);
                vis               = gate_sum(ctx, vis, vis_from_aud, param_chunk(ctx, av, 2 * dim_a, dim));
                aud               = gate_sum(ctx, aud, aud_from_vis, param_chunk(ctx, va, 2 * dim, dim_a));

                h   = scale_shift(ctx, plain_norm(ctx, aud, eps), param_chunk(ctx, ma, 7 * dim_a, dim_a), param_chunk(ctx, ma, 6 * dim_a, dim_a));
                aud = gate_sum(ctx, aud, a_ff->forward(ctx, h), param_chunk(ctx, ma, 8 * dim_a, dim_a));
            } else {
                vis = gate_sum(ctx, vis, vis_out_t, gate_v);
            }

            h   = scale_shift(ctx, plain_norm(ctx, vis, eps), param_chunk(ctx, mv, 7 * dim, dim), param_chunk(ctx, mv, 6 * dim, dim));
            vis = gate_sum(ctx, vis, v_ff->forward(ctx, h), param_chunk(ctx, mv, 8 * dim, dim));
            return {vis, aud};
        }
    };

    class OutLayer : public GGMLBlock {
        int64_t dim;
        float eps;
        bool double_norm;

    public:
        OutLayer(int64_t dim, int64_t time_dim, int64_t out_dim, float eps, bool double_norm)
            : dim(dim), eps(eps), double_norm(double_norm) {
            blocks["modulation"] = std::make_shared<Modulation>(time_dim, 2 * dim);
            blocks["out_layer"]  = std::make_shared<Linear>(dim, out_dim);
        }

        ggml_tensor* forward(GGMLRunnerContext* ctx, ggml_tensor* x, ggml_tensor* t) {
            auto modulation = std::dynamic_pointer_cast<Modulation>(blocks["modulation"]);
            auto out_layer  = std::dynamic_pointer_cast<Linear>(blocks["out_layer"]);
            auto m          = modulation->forward(ctx, t);  // [shift, scale]
            x               = scale_shift(ctx, plain_norm(ctx, x, eps), param_chunk(ctx, m, dim, dim), param_chunk(ctx, m, 0, dim));
            if (double_norm) {
                // The reference audio head normalizes again after the affine.
                x = plain_norm(ctx, x, eps);
            }
            return out_layer->forward(ctx, x);
        }
    };

    struct Kandinsky6Output {
        ggml_tensor* video = nullptr;  // [W, H, T, out_visual_dim]
        ggml_tensor* audio = nullptr;  // [L, out_audio_dim]
    };

    class Kandinsky6Model : public GGMLBlock {
    public:
        Kandinsky6Config config;

        Kandinsky6Model() = default;
        Kandinsky6Model(const Kandinsky6Config& config)
            : config(config) {
            const int64_t patch_volume  = config.patch_size[0] * config.patch_size[1] * config.patch_size[2];
            blocks["visual_embeddings.in_layer"] = std::make_shared<Linear>(patch_volume * config.visual_in_channels(), config.model_dim);
            blocks["out_layer"]                  = std::make_shared<OutLayer>(config.model_dim, config.time_dim, patch_volume * config.out_visual_dim, config.norm_eps, false);
            blocks["audio_embeddings"]           = std::make_shared<TextEmbeddings>(config.in_audio_dim, config.model_dim_a, config.norm_eps);
            blocks["audio_out_layer"]            = std::make_shared<OutLayer>(config.model_dim_a, config.time_dim_a, config.out_audio_dim, config.norm_eps, true);
            for (const auto& [prefix, dim, time_dim, ff_dim] : {std::make_tuple(std::string("video"), config.model_dim, config.time_dim, config.ff_dim),
                                                                std::make_tuple(std::string("audio"), config.model_dim_a, config.time_dim_a, config.ff_dim_a)}) {
                blocks[prefix + "_time_embeddings"]        = std::make_shared<TimeEmbeddings>(dim, time_dim);
                blocks[prefix + "_text_embeddings"]        = std::make_shared<TextEmbeddings>(config.in_text_dim, dim, config.norm_eps);
                blocks[prefix + "_pooled_text_embeddings"] = std::make_shared<TextEmbeddings>(config.in_text_dim2, time_dim, config.norm_eps);
                for (int i = 0; i < config.num_text_blocks; ++i) {
                    blocks[prefix + "_text_transformer_blocks." + std::to_string(i)] =
                        std::make_shared<TextEncoderBlock>(dim, time_dim, ff_dim, config.head_dim, config.qk_norm_eps, config.norm_eps);
                }
            }
            for (int i = 0; i < config.num_visual_blocks; ++i) {
                blocks["visual_transformer_blocks." + std::to_string(i)] = std::make_shared<FusedBlock>(config);
            }
        }

        // x: [W, H, T, C] -> [N=1, tokens, pt*ph*pw*C] with tokens in (t, h, w) order and features in (ph, pw, c) order.
        ggml_tensor* patchify(GGMLRunnerContext* ctx, ggml_tensor* x) {
            auto c          = ctx->ggml_ctx;
            const int64_t W = x->ne[0], H = x->ne[1], T = x->ne[2], C = x->ne[3];
            const int64_t Wp = W / 2, Hp = H / 2;
            x = ggml_reshape_4d(c, x, 2, Wp, 2, Hp * T * C);         // [pw, Wp, ph, Hp*T*C]
            x = ggml_cont(c, ggml_permute(c, x, 0, 2, 1, 3));        // [pw, ph, Wp, Hp*T*C]
            x = ggml_reshape_4d(c, x, 4, Wp, Hp * T, C);              // [4, Wp, Hp*T, C]
            x = ggml_cont(c, ggml_permute(c, x, 1, 2, 3, 0));        // [C, 4, Wp, Hp*T]
            return ggml_reshape_3d(c, x, 4 * C, Wp * Hp * T, 1);     // [4C, tokens, 1]
        }

        // [N=1, tokens, C*ph*pw] (features in (c, ph, pw) order) -> [W, H, T, C]
        ggml_tensor* unpatchify(GGMLRunnerContext* ctx, ggml_tensor* x, int64_t Wp, int64_t Hp, int64_t T) {
            auto c          = ctx->ggml_ctx;
            const int64_t C = x->ne[0] / 4;
            x = ggml_reshape_4d(c, x, 4, C, Wp, Hp * T);              // [4, C, Wp, Hp*T]
            x = ggml_cont(c, ggml_permute(c, x, 0, 3, 1, 2));        // [4, Wp, Hp*T, C]
            x = ggml_reshape_4d(c, x, 2, 2, Wp, Hp * T * C);          // [pw, ph, Wp, Hp*T*C]
            x = ggml_cont(c, ggml_permute(c, x, 0, 2, 1, 3));        // [pw, Wp, ph, Hp*T*C]
            return ggml_reshape_4d(c, x, Wp * 2, Hp * 2, T, C);
        }

        Kandinsky6Output forward(GGMLRunnerContext* ctx,
                                 ggml_tensor* x,          // [W, H, T, visual_in_channels]
                                 ggml_tensor* timesteps,  // [1]
                                 ggml_tensor* context,    // [L_text, in_text_dim]
                                 ggml_tensor* pooled,     // [in_text_dim2]
                                 ggml_tensor* audio_x,    // [L_audio, in_audio_dim] or null
                                 ggml_tensor* text_pe,
                                 ggml_tensor* vis_pe,
                                 ggml_tensor* aud_pe) {
            GGML_ASSERT(x->ne[3] == config.visual_in_channels());
            const int64_t W = x->ne[0], H = x->ne[1], T = x->ne[2];
            GGML_ASSERT(W % 2 == 0 && H % 2 == 0);
            auto block = [&](const std::string& name) { return blocks[name]; };

            context = ggml_reshape_3d(ctx->ggml_ctx, context, context->ne[0], context->ne[1], 1);
            pooled  = ggml_reshape_2d(ctx->ggml_ctx, pooled, pooled->ne[0], 1);

            ggml_tensor* text_embed[2];
            ggml_tensor* temb[2];
            const char* branches[2] = {"video", "audio"};
            for (int b = 0; b < 2; ++b) {
                const std::string p = branches[b];
                auto text_layer     = std::dynamic_pointer_cast<TextEmbeddings>(block(p + "_text_embeddings"));
                auto time_layer     = std::dynamic_pointer_cast<TimeEmbeddings>(block(p + "_time_embeddings"));
                auto pooled_layer   = std::dynamic_pointer_cast<TextEmbeddings>(block(p + "_pooled_text_embeddings"));
                temb[b]             = ggml_add(ctx->ggml_ctx, time_layer->forward(ctx, timesteps), pooled_layer->forward(ctx, pooled));
                text_embed[b]       = text_layer->forward(ctx, context);
                for (int i = 0; i < config.num_text_blocks; ++i) {
                    auto text_block = std::dynamic_pointer_cast<TextEncoderBlock>(block(p + "_text_transformer_blocks." + std::to_string(i)));
                    text_embed[b]   = text_block->forward(ctx, text_embed[b], temb[b], text_pe);
                }
            }

            auto vis_in = std::dynamic_pointer_cast<Linear>(block("visual_embeddings.in_layer"));
            auto vis    = vis_in->forward(ctx, patchify(ctx, x));  // [1, tokens, model_dim]

            ggml_tensor* aud = nullptr;
            if (audio_x != nullptr) {
                auto audio_layer = std::dynamic_pointer_cast<TextEmbeddings>(block("audio_embeddings"));
                aud              = audio_layer->forward(ctx, ggml_reshape_3d(ctx->ggml_ctx, audio_x, audio_x->ne[0], audio_x->ne[1], 1));
            }

            for (int i = 0; i < config.num_visual_blocks; ++i) {
                auto fused       = std::dynamic_pointer_cast<FusedBlock>(block("visual_transformer_blocks." + std::to_string(i)));
                std::tie(vis, aud) = fused->forward(ctx, vis, aud, text_embed[0], text_embed[1], temb[0], temb[1], vis_pe, aud_pe);
            }

            Kandinsky6Output out;
            auto video_head = std::dynamic_pointer_cast<OutLayer>(block("out_layer"));
            out.video       = unpatchify(ctx, video_head->forward(ctx, vis, temb[0]), W / 2, H / 2, T);
            if (aud != nullptr) {
                auto audio_head = std::dynamic_pointer_cast<OutLayer>(block("audio_out_layer"));
                out.audio       = audio_head->forward(ctx, aud, temb[1]);
                out.audio       = ggml_reshape_2d(ctx->ggml_ctx, out.audio, out.audio->ne[0], out.audio->ne[1]);
            }
            return out;
        }
    };

    // RoPE tables in the [L, dim/2, 2, 2] = [[cos, -sin], [sin, cos]] layout of the reference.
    static void push_rotation(std::vector<float>& out, float angle) {
        const float c = std::cos(angle), s = std::sin(angle);
        out.insert(out.end(), {c, -s, s, c});
    }

    static std::vector<float> make_freqs(int count, float theta, float scaling = 1.f) {
        std::vector<float> freqs(static_cast<size_t>(count));
        for (int i = 0; i < count; ++i) {
            freqs[static_cast<size_t>(i)] = std::exp(-std::log(theta) * static_cast<float>(i) / static_cast<float>(count)) * scaling;
        }
        return freqs;
    }

    static sd::Tensor<float> make_rope_1d(int64_t length, int64_t head_dim, float theta, float scaling) {
        const auto freqs = make_freqs(static_cast<int>(head_dim / 2), theta, scaling);
        std::vector<float> values;
        values.reserve(static_cast<size_t>(length * head_dim * 2));
        for (int64_t pos = 0; pos < length; ++pos) {
            for (float freq : freqs) {
                push_rotation(values, static_cast<float>(pos) * freq);
            }
        }
        return sd::Tensor<float>({2, 2, head_dim / 2, length}, std::move(values));
    }

    static sd::Tensor<float> make_rope_3d(int64_t t_len, int64_t h_len, int64_t w_len, const Kandinsky6Config& config) {
        const auto ft = make_freqs(config.axes_dims[0] / 2, config.rope_theta);
        const auto fh = make_freqs(config.axes_dims[1] / 2, config.rope_theta);
        const auto fw = make_freqs(config.axes_dims[2] / 2, config.rope_theta);
        const int64_t half = config.head_dim / 2;
        std::vector<float> values;
        values.reserve(static_cast<size_t>(t_len * h_len * w_len * half * 4));
        for (int64_t t = 0; t < t_len; ++t) {
            for (int64_t h = 0; h < h_len; ++h) {
                for (int64_t w = 0; w < w_len; ++w) {
                    for (float f : ft) {
                        push_rotation(values, static_cast<float>(t) * f / config.rope_scale[0]);
                    }
                    for (float f : fh) {
                        push_rotation(values, static_cast<float>(h) * f / config.rope_scale[1]);
                    }
                    for (float f : fw) {
                        push_rotation(values, static_cast<float>(w) * f / config.rope_scale[2]);
                    }
                }
            }
        }
        return sd::Tensor<float>({2, 2, half, t_len * h_len * w_len}, std::move(values));
    }

    // One PiFlow step (network-free DX policy rollout of diffusers' PiflowScheduler) for one modality.
    // grid[g] holds the model's velocity prediction g in x's memory layout. Returns x at sigma_dst,
    // where sigma_dst == 0 ends the rollout at raw time eps like the reference.
    static std::vector<float> piflow_rollout(const float* x,
                                             const std::vector<const float*>& grid,
                                             size_t n,
                                             float sigma_src,
                                             float sigma_dst,
                                             const Kandinsky6Config& config) {
        const float shift  = config.piflow_shift;
        const float eps    = config.piflow_eps;
        auto unwarp        = [&](float sigma) { return sigma / (shift + (1.f - shift) * sigma); };
        auto warp          = [&](float raw) { return shift * raw / (1.f + (shift - 1.f) * raw); };
        const float raw_src = unwarp(sigma_src);
        const float raw_dst = sigma_dst > 0.f ? unwarp(sigma_dst) : 0.f;
        const float segment = std::max(raw_src - raw_dst, 0.f);
        const float seg_size = std::max(raw_src - raw_dst, eps);
        const float raw_end  = std::max(raw_src - segment, eps);
        const float delta    = raw_src - raw_end;
        const int substeps   = std::max(1, static_cast<int>(std::nearbyint(delta * static_cast<float>(config.piflow_substeps))));
        const float substep  = delta / static_cast<float>(substeps);
        const int n_grid     = static_cast<int>(grid.size());

        std::vector<float> x_t(x, x + n);
        float sigma_t = sigma_src;
        float raw_t   = raw_src;
        for (int s = 0; s < substeps; ++s) {
            const float tt   = std::clamp((unwarp(sigma_t) - raw_dst) / seg_size, 0.f, 1.f) * static_cast<float>(n_grid - 1);
            const int t0     = std::clamp(static_cast<int>(std::floor(tt)), 0, n_grid - 2);
            const float w0   = static_cast<float>(t0 + 1) - tt;
            const float w1   = tt - static_cast<float>(t0);
            const float* v0  = grid[static_cast<size_t>(t0)];
            const float* v1  = grid[static_cast<size_t>(t0 + 1)];
            const float inv  = 1.f / std::max(sigma_t, eps);
            const float raw_next   = std::max(raw_t - substep, 0.f);
            const float sigma_next = warp(raw_next);
            const float dt         = sigma_next - sigma_t;
            for (size_t i = 0; i < n; ++i) {
                const float x0 = w0 * (x[i] - sigma_src * v0[i]) + w1 * (x[i] - sigma_src * v1[i]);
                x_t[i] += (x_t[i] - x0) * inv * dt;
            }
            sigma_t = sigma_next;
            raw_t   = raw_next;
        }
        return x_t;
    }

    struct Kandinsky6Result {
        sd::Tensor<float> video;  // [W, H, T, out_visual_dim]
        sd::Tensor<float> audio;  // [out_audio_dim, L]
    };

    struct Kandinsky6Runner : public DiffusionModelRunner {
        Kandinsky6Config config;
        Kandinsky6Model model;
        // make_input copies after graph allocation, so graph inputs built here must outlive build_graph.
        sd::Tensor<float> text_pe_, vis_pe_, aud_pe_, zero_cond_;

        Kandinsky6Runner(ggml_backend_t backend,
                         const String2TensorStorage& tensor_storage_map      = {},
                         const std::string prefix                            = "",
                         std::shared_ptr<RunnerWeightManager> weight_manager = nullptr)
            : DiffusionModelRunner(backend, prefix, weight_manager),
              config(Kandinsky6Config::detect_from_weights(tensor_storage_map, prefix)),
              model(config) {
            model.init(params_ctx, tensor_storage_map, prefix);
        }

        std::string get_desc() override {
            return "kandinsky6";
        }

        int get_patch_size() const override {
            return config.patch_size[1];
        }

        void get_param_tensors(std::map<std::string, ggml_tensor*>& tensors, const std::string& prefix) override {
            model.get_param_tensors(tensors, prefix);
        }

        ggml_cgraph* build_graph(const sd::Tensor<float>& x_tensor,
                                 const sd::Tensor<float>& timesteps_tensor,
                                 const sd::Tensor<float>& context_tensor,
                                 const sd::Tensor<float>& pooled_tensor,
                                 const sd::Tensor<float>& audio_tensor,
                                 const sd::Tensor<float>& c_concat_tensor) {
            ggml_cgraph* gf = new_graph_custom(KANDINSKY6_GRAPH_SIZE);
            auto x          = make_input(x_tensor);
            auto timesteps  = make_input(timesteps_tensor);
            auto context    = make_input(context_tensor);
            auto pooled     = make_input(pooled_tensor);
            auto audio_x    = make_optional_input(audio_tensor);

            const int64_t W = x->ne[0], H = x->ne[1], T = x->ne[2];
            if (x->ne[3] == config.in_visual_dim) {
                // T2V: the conditioning latent and mask channels are zero.
                ggml_tensor* cond;
                if (c_concat_tensor.empty()) {
                    zero_cond_ = sd::Tensor<float>::zeros({W, H, T, config.in_visual_dim + 1});
                    cond       = make_input(zero_cond_);
                } else {
                    cond = make_input(c_concat_tensor);
                }
                x = ggml_concat(compute_ctx, x, cond, 3);
            }

            text_pe_     = make_rope_1d(context->ne[1], config.head_dim, config.rope_theta, 1.f);
            vis_pe_      = make_rope_3d(T, H / 2, W / 2, config);
            auto text_pe = make_input(text_pe_);
            auto vis_pe  = make_input(vis_pe_);
            ggml_tensor* aud_pe = nullptr;
            if (audio_x != nullptr) {
                aud_pe_ = make_rope_1d(audio_x->ne[1], config.head_dim, config.rope_theta, config.audio_freqs_scaling);
                aud_pe  = make_input(aud_pe_);
            }

            auto runner_ctx = get_context();
            auto out        = model.forward(&runner_ctx, x, timesteps, context, pooled, audio_x, text_pe, vis_pe, aud_pe);
            ggml_tensor* result = ggml_reshape_1d(compute_ctx, out.video, ggml_nelements(out.video));
            if (out.audio != nullptr) {
                result = ggml_concat(compute_ctx, result, ggml_reshape_1d(compute_ctx, ggml_cont(compute_ctx, out.audio), ggml_nelements(out.audio)), 0);
            }
            ggml_build_forward_expand(gf, result);
            return gf;
        }

        Kandinsky6Result compute_av(int n_threads,
                                    const sd::Tensor<float>& x,
                                    const sd::Tensor<float>& timesteps,
                                    const sd::Tensor<float>& context,
                                    const sd::Tensor<float>& pooled,
                                    const sd::Tensor<float>& audio_x  = {},
                                    const sd::Tensor<float>& c_concat = {}) {
            auto get_graph = [&]() -> ggml_cgraph* {
                return build_graph(x, timesteps, context, pooled, audio_x, c_concat);
            };
            auto flat = GGMLRunner::compute(get_graph, n_threads, false);
            if (!flat.has_value() || flat->empty()) {
                return {};
            }
            Kandinsky6Result result;
            result.video        = sd::Tensor<float>({x.shape()[0], x.shape()[1], x.shape()[2], config.out_visual_dim});
            const size_t video_n = static_cast<size_t>(result.video.numel());
            std::copy_n(flat->data(), video_n, result.video.data());
            if (!audio_x.empty()) {
                result.audio = sd::Tensor<float>({config.out_audio_dim, audio_x.shape()[1]});
                std::copy_n(flat->data() + video_n, static_cast<size_t>(result.audio.numel()), result.audio.data());
            }
            return result;
        }

        // The sampler state packs the video latent [W, H, T, C] and, in extra channels, the audio
        // latent laid out as [L, C_audio] (as audio_latent_shape describes it). The distilled model
        // predicts n_grid velocities per channel; the PiFlow policy turns them into the sample at the
        // step's target sigma. The returned flow velocity (x - x_next) / (sigma - sigma_next) makes a
        // plain Euler step land exactly on that sample.
        sd::Tensor<float> compute(int n_threads, const DiffusionParams& diffusion_params) override {
            GGML_ASSERT(diffusion_params.x != nullptr && diffusion_params.timesteps != nullptr);
            if (diffusion_params.context == nullptr || diffusion_params.y == nullptr) {
                throw std::runtime_error("Kandinsky 6 requires Qwen2.5-VL token embeddings and a CLIP-L pooled vector");
            }
            const auto* extra = diffusion_extra_as<Kandinsky6DiffusionExtra>(diffusion_params);
            if (config.is_piflow() && !std::isfinite(extra->target_sigma)) {
                throw std::runtime_error("Kandinsky 6 PiFlow sampling needs the step's target sigma; use the euler method");
            }
            const auto& x            = *diffusion_params.x;
            const int64_t W          = x.shape()[0];
            const int64_t H          = x.shape()[1];
            const int64_t T          = x.shape()[2];
            const int64_t C          = config.in_visual_dim;
            const int64_t spatial    = W * H * T;
            const int64_t channels   = x.shape()[3];
            const int64_t audio_len  = extra->audio_length;
            const int64_t audio_dim  = config.in_audio_dim;
            const size_t video_n     = static_cast<size_t>(C * spatial);
            const size_t audio_n     = static_cast<size_t>(audio_len * audio_dim);
            const float sigma        = diffusion_params.timesteps->data()[0] / 1000.f;
            const float sigma_target = extra->target_sigma;
            if (channels < C || (channels - C) * spatial < static_cast<int64_t>(audio_n) ||
                (config.is_piflow() && sigma <= sigma_target)) {
                throw std::runtime_error("Kandinsky 6 latent does not match its audio length or schedule");
            }

            sd::Tensor<float> video({W, H, T, C});
            std::copy_n(x.data(), video_n, video.data());
            sd::Tensor<float> audio_tl;  // [L, C_audio] as packed in the sampler state
            sd::Tensor<float> audio;     // [C_audio, L] as the model consumes it
            if (audio_n > 0) {
                audio_tl = sd::Tensor<float>({audio_len, audio_dim});
                audio    = sd::Tensor<float>({audio_dim, audio_len});
                std::copy_n(x.data() + video_n, audio_n, audio_tl.data());
                for (int64_t c = 0; c < audio_dim; ++c) {
                    for (int64_t l = 0; l < audio_len; ++l) {
                        audio.data()[l * audio_dim + c] = audio_tl.data()[c * audio_len + l];
                    }
                }
            }

            auto out = compute_av(n_threads, video, *diffusion_params.timesteps, *diffusion_params.context,
                                  *diffusion_params.y, audio, tensor_or_empty(diffusion_params.c_concat));
            if (out.video.empty() || (audio_n > 0 && out.audio.empty())) {
                return {};
            }

            sd::Tensor<float> result(x.shape());
            std::fill_n(result.data(), static_cast<size_t>(result.numel()), 0.f);
            if (!config.is_piflow()) {
                // Plain flow velocity: video [W, H, T, C] as is, audio [C_audio, L] -> packed [L, C_audio].
                std::copy_n(out.video.data(), video_n, result.data());
                for (int64_t c = 0; c < audio_dim && audio_n > 0; ++c) {
                    for (int64_t l = 0; l < audio_len; ++l) {
                        result.data()[video_n + static_cast<size_t>(c * audio_len + l)] = out.audio.data()[l * audio_dim + c];
                    }
                }
                return result;
            }

            const int64_t n_grid = config.n_grid();
            const float dt_inv   = 1.f / (sigma - sigma_target);

            std::vector<const float*> grid;
            for (int64_t g = 0; g < n_grid; ++g) {
                grid.push_back(out.video.data() + g * static_cast<int64_t>(video_n));
            }
            auto next = piflow_rollout(video.data(), grid, video_n, sigma, sigma_target, config);
            for (size_t i = 0; i < video_n; ++i) {
                result.data()[i] = (video.data()[i] - next[i]) * dt_inv;
            }

            if (audio_n > 0) {
                // Model output [n_grid * C_audio, L] -> per-grid arrays in the packed [L, C_audio] layout.
                std::vector<float> audio_grid(static_cast<size_t>(n_grid) * audio_n);
                const int64_t out_dim = config.out_audio_dim;
                for (int64_t g = 0; g < n_grid; ++g) {
                    for (int64_t c = 0; c < audio_dim; ++c) {
                        for (int64_t l = 0; l < audio_len; ++l) {
                            audio_grid[static_cast<size_t>(g) * audio_n + static_cast<size_t>(c * audio_len + l)] =
                                out.audio.data()[l * out_dim + g * audio_dim + c];
                        }
                    }
                }
                grid.clear();
                for (int64_t g = 0; g < n_grid; ++g) {
                    grid.push_back(audio_grid.data() + static_cast<size_t>(g) * audio_n);
                }
                next = piflow_rollout(audio_tl.data(), grid, audio_n, sigma, sigma_target, config);
                for (size_t i = 0; i < audio_n; ++i) {
                    result.data()[video_n + i] = (audio_tl.data()[i] - next[i]) * dt_inv;
                }
            }
            return result;
        }
    };
}  // namespace Kandinsky6

#endif  // __SD_MODEL_DIFFUSION_KANDINSKY6_HPP__
