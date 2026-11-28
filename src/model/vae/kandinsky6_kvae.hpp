#ifndef __SD_MODEL_VAE_KANDINSKY6_KVAE_HPP__
#define __SD_MODEL_VAE_KANDINSKY6_KVAE_HPP__

#include "core/ggml_graph_cut.h"
#include "model/vae/seedvr2_vae.hpp"
#include "model/vae/vae.hpp"
#include "model/vae/wan_vae.hpp"

// Causal 3D K-VAE of the Kandinsky 6.0 video super-resolution models (Kandinsky6SRVAE in diffusers):
// 64 latent channels, 16x spatial and 4x temporal compression, RMS-normalized causal resnets, and a
// decoder whose norms are modulated by the latent (SpatialNorm). Tensors are [W, H, T, C].
//
// The decoder runs one latent frame per graph and carries every causal convolution's padding in
// the runner's cache tensors, which reproduces a single pass exactly (the diffusers segmentation
// relies on the same property) while bounding memory to a few output frames.
namespace Kandinsky6KVAE {
    constexpr float SCALING_FACTOR = 0.910344004631042f;

    // Per-graph causal state. In streaming mode every causal convolution reads the frames the
    // previous segment left in cache slot "kvae.<n>" and stores the frames the next one needs.
    struct Causal {
        GGMLRunnerContext* ctx = nullptr;
        bool streaming         = false;
        bool first             = true;
        int index              = 0;
    };

    // Conv3d with causal time padding: the first segment repeats its first frame, later ones use
    // the cached tail of the previous segment. Height and width are zero padded inside the conv.
    class CausalConv : public UnaryBlock {
        int time_pad;

    public:
        CausalConv(int in, int out, std::tuple<int, int, int> kernel, std::tuple<int, int, int> stride = {1, 1, 1})
            : time_pad(std::get<0>(kernel) - 1) {
            blocks["conv"] = std::make_shared<SeedVR2::InflatedCausalConv3d>(
                in, out, kernel, stride, std::tuple<int, int, int>{0, std::get<1>(kernel) / 2, std::get<2>(kernel) / 2});
        }

        ggml_tensor* forward(GGMLRunnerContext* ctx, ggml_tensor* x) override {
            return std::dynamic_pointer_cast<UnaryBlock>(blocks["conv"])->forward(ctx, x);
        }

        ggml_tensor* forward(Causal& state, ggml_tensor* x) {
            auto c = state.ctx->ggml_ctx;
            if (time_pad > 0) {
                const std::string name = "kvae." + std::to_string(state.index++);
                ggml_tensor* head      = state.first ? nullptr : state.ctx->load_cache_tensor(name);
                if (head == nullptr) {
                    auto first = ggml_ext_slice(state.ctx, x, 2, 0, 1);
                    head       = ggml_repeat(c, first, ggml_new_tensor_4d(c, x->type, x->ne[0], x->ne[1], time_pad, x->ne[3]));
                }
                x = ggml_concat(c, head, x, 2);
                if (state.streaming)
                    state.ctx->persist_cache_tensor(name, ggml_ext_slice(state.ctx, x, 2, x->ne[2] - time_pad, x->ne[2]));
            }
            return forward(state.ctx, x);
        }
    };

    // Pointwise Conv3d (kernel 1); weights keep the [out, in, 1, 1, 1] checkpoint layout.
    static std::shared_ptr<SeedVR2::InflatedCausalConv3d> pointwise(int in, int out) {
        return std::make_shared<SeedVR2::InflatedCausalConv3d>(in, out, std::tuple<int, int, int>{1, 1, 1}, std::tuple<int, int, int>{1, 1, 1},
                                                               std::tuple<int, int, int>{0, 0, 0});
    }

    static ggml_tensor* apply(GGMLRunnerContext* ctx, const std::shared_ptr<GGMLBlock>& block, ggml_tensor* x) {
        return std::dynamic_pointer_cast<UnaryBlock>(block)->forward(ctx, x);
    }

    // zq [w, h, t, Cz] nearest-resized to the feature grid. In the first segment latent frame 0
    // maps to feature frame 0 alone and the rest of the latent fills the remaining frames.
    static ggml_tensor* resize_zq(Causal& state, ggml_tensor* zq, int64_t W, int64_t H, int64_t T) {
        auto c       = state.ctx->ggml_ctx;
        auto nearest = [&](ggml_tensor* z, int64_t frames) {
            if (z->ne[0] == W && z->ne[1] == H && z->ne[2] == frames)
                return z;
            return ggml_interpolate(c, ggml_ext_cont(state.ctx, z), W, H, frames, z->ne[3], GGML_SCALE_MODE_NEAREST);
        };
        if (!state.first)
            return nearest(zq, T);
        auto first = nearest(ggml_ext_slice(state.ctx, zq, 2, 0, 1), 1);
        if (zq->ne[2] == 1 || T == 1)
            return first;
        return ggml_concat(c, first, nearest(ggml_ext_slice(state.ctx, zq, 2, 1, zq->ne[2]), T - 1), 2);
    }

    class SpatialNorm : public GGMLBlock {
    public:
        SpatialNorm(int channels, int zq_channels) {
            blocks["norm_layer"] = std::make_shared<WAN::RMS_norm>(channels);
            blocks["conv_y"]     = pointwise(zq_channels, channels);
            blocks["conv_b"]     = pointwise(zq_channels, channels);
        }

        ggml_tensor* forward(Causal& state, ggml_tensor* x, ggml_tensor* zq) {
            auto ctx = state.ctx;
            // The pointwise convolutions commute with nearest resizing, so they run at latent size.
            auto y = resize_zq(state, apply(ctx, blocks["conv_y"], zq), x->ne[0], x->ne[1], x->ne[2]);
            auto b = resize_zq(state, apply(ctx, blocks["conv_b"], zq), x->ne[0], x->ne[1], x->ne[2]);
            x      = apply(ctx, blocks["norm_layer"], x);
            return ggml_add(ctx->ggml_ctx, ggml_mul(ctx->ggml_ctx, x, y), b);
        }
    };

    class ResnetBlock : public GGMLBlock {
        int in_channels, out_channels;
        bool modulated;

    public:
        ResnetBlock(int in, int out, int zq_channels = 0)
            : in_channels(in), out_channels(out), modulated(zq_channels > 0) {
            if (modulated) {
                blocks["norm1"] = std::make_shared<SpatialNorm>(in, zq_channels);
                blocks["norm2"] = std::make_shared<SpatialNorm>(out, zq_channels);
            } else {
                blocks["norm1"] = std::make_shared<WAN::RMS_norm>(in);
                blocks["norm2"] = std::make_shared<WAN::RMS_norm>(out);
            }
            blocks["conv1"] = std::make_shared<CausalConv>(in, out, std::tuple<int, int, int>{3, 3, 3});
            blocks["conv2"] = std::make_shared<CausalConv>(out, out, std::tuple<int, int, int>{3, 3, 3});
            if (in != out)
                blocks["nin_shortcut"] = pointwise(in, out);
        }

        ggml_tensor* forward(Causal& state, ggml_tensor* x, ggml_tensor* zq = nullptr) {
            auto ctx  = state.ctx;
            auto norm = [&](const char* name, ggml_tensor* h) {
                if (modulated)
                    return std::dynamic_pointer_cast<SpatialNorm>(blocks[name])->forward(state, h, zq);
                return apply(ctx, blocks[name], h);
            };
            auto h = ggml_silu(ctx->ggml_ctx, norm("norm1", x));
            h      = std::dynamic_pointer_cast<CausalConv>(blocks["conv1"])->forward(state, h);
            h      = ggml_silu(ctx->ggml_ctx, norm("norm2", h));
            h      = std::dynamic_pointer_cast<CausalConv>(blocks["conv2"])->forward(state, h);
            if (in_channels != out_channels)
                x = apply(ctx, blocks["nin_shortcut"], x);
            return ggml_add(ctx->ggml_ctx, x, h);
        }
    };

    class Downsample : public GGMLBlock {
        bool compress_time;

    public:
        Downsample(int in, bool compress_time)
            : compress_time(compress_time) {
            const int out           = 2 * in;
            blocks["spatial_conv"] = std::make_shared<SeedVR2::InflatedCausalConv3d>(
                in, out, std::tuple<int, int, int>{1, 3, 3}, std::tuple<int, int, int>{1, 2, 2}, std::tuple<int, int, int>{0, 1, 1});
            if (compress_time) {
                blocks["temporal_conv.0"] = std::make_shared<CausalConv>(out, out, std::tuple<int, int, int>{2, 1, 1});
                blocks["temporal_conv.1"] = std::make_shared<CausalConv>(out, out, std::tuple<int, int, int>{2, 1, 1}, std::tuple<int, int, int>{2, 1, 1});
            }
            blocks["linear"] = pointwise(out, out);
        }

        // Single-pass (first segment) only: the encoder always sees the whole clip.
        ggml_tensor* forward(Causal& state, ggml_tensor* x) {
            auto ctx        = state.ctx;
            auto c          = ctx->ggml_ctx;
            const int64_t W = x->ne[0], H = x->ne[1], T = x->ne[2], C = x->ne[3];
            // Pixel-unshuffle, then average the two horizontal sub-pixels: channel 2c + dy.
            auto pooled = ggml_pool_2d(c, ggml_reshape_4d(c, ggml_ext_cont(ctx, x), W, H, T * C, 1), GGML_OP_POOL_AVG, 2, 1, 2, 1, 0, 0);
            pooled      = ggml_reshape_4d(c, pooled, W / 2, 2, (H / 2) * T, C);
            pooled      = ggml_cont(c, ggml_permute(c, pooled, 0, 2, 1, 3));  // [W/2, H/2*T, 2, C]
            pooled      = ggml_reshape_4d(c, pooled, W / 2, H / 2, T, 2 * C);
            x           = ggml_add(c, apply(ctx, blocks["spatial_conv"], x), pooled);
            if (!compress_time)
                return apply(ctx, blocks["linear"], x);

            // Average frame pairs; the first frame stays on its own.
            const int64_t t = x->ne[2];
            GGML_ASSERT(t % 2 == 1);
            ggml_tensor* avg = ggml_ext_slice(ctx, x, 2, 0, 1);
            if (t > 1) {
                auto rest = ggml_ext_slice(ctx, x, 2, 1, t);
                auto view = [&](int offset) {
                    auto v = ggml_view_4d(c, rest, rest->ne[0], rest->ne[1], (t - 1) / 2, rest->ne[3], rest->nb[1], rest->nb[2] * 2,
                                          rest->nb[3], offset * rest->nb[2]);
                    return ggml_cont(c, v);
                };
                avg = ggml_concat(c, avg, ggml_scale(c, ggml_add(c, view(0), view(1)), 0.5f), 2);
            }
            auto h = std::dynamic_pointer_cast<CausalConv>(blocks["temporal_conv.0"])->forward(state, x);
            h      = std::dynamic_pointer_cast<CausalConv>(blocks["temporal_conv.1"])->forward(state, h);
            return apply(ctx, blocks["linear"], ggml_add(c, h, avg));
        }
    };

    class Upsample : public GGMLBlock {
        bool compress_time;

    public:
        Upsample(int channels, bool compress_time)
            : compress_time(compress_time) {
            blocks["spatial_conv"] = std::make_shared<SeedVR2::InflatedCausalConv3d>(
                channels, channels, std::tuple<int, int, int>{1, 3, 3}, std::tuple<int, int, int>{1, 1, 1}, std::tuple<int, int, int>{0, 1, 1});
            if (compress_time)
                blocks["temporal_conv"] = std::make_shared<CausalConv>(channels, channels, std::tuple<int, int, int>{3, 1, 1});
            blocks["linear"] = pointwise(channels, channels);
        }

        ggml_tensor* forward(Causal& state, ggml_tensor* x) {
            auto ctx = state.ctx;
            auto c   = ctx->ggml_ctx;
            if (compress_time) {
                // Repeat every frame; the first segment drops the extra copy of its first frame.
                const int64_t W = x->ne[0], H = x->ne[1], T = x->ne[2], C = x->ne[3];
                auto rep        = ggml_reshape_4d(c, ggml_ext_cont(ctx, x), W * H, 1, T, C);
                rep             = ggml_repeat(c, rep, ggml_new_tensor_4d(c, x->type, W * H, 2, T, C));
                rep             = ggml_reshape_4d(c, rep, W, H, 2 * T, C);
                if (state.first)
                    rep = ggml_ext_slice(ctx, rep, 2, 1, 2 * T);
                x = ggml_add(c, std::dynamic_pointer_cast<CausalConv>(blocks["temporal_conv"])->forward(state, rep), rep);
            }
            x = ggml_upscale(c, ggml_ext_cont(ctx, x), 2, GGML_SCALE_MODE_NEAREST);
            x = ggml_add(c, x, apply(ctx, blocks["spatial_conv"], x));
            return apply(ctx, blocks["linear"], x);
        }
    };

    struct KVAEConfig {
        std::vector<int> encoder_channels{16, 128, 256, 512, 1024};
        std::vector<int> decoder_channels{16, 256, 512, 1024, 2048};
        int latent_channels  = 64;
        int layers           = 2;
        int time_start_level = 1;
        int time_levels      = 2;

        static KVAEConfig detect_from_weights(const String2TensorStorage& weights, const std::string& prefix) {
            KVAEConfig config;
            auto ne = [&](const std::string& key, int dim) -> int64_t {
                auto it = weights.find((prefix.empty() ? "" : prefix + ".") + key);
                return it == weights.end() ? -1 : it->second.ne[dim];
            };
            for (int level = 0; level < 5; ++level) {
                config.encoder_channels[level] = int(ne("encoder.down." + std::to_string(level) + ".block.0.conv2.conv.bias", 0));
                config.decoder_channels[level] = int(ne("decoder.up." + std::to_string(level) + ".block.0.conv2.conv.bias", 0));
            }
            // Conv3d weights load flattened as [kw, kh, kt, in * out].
            config.latent_channels = int(ne("decoder.mid.block_1.norm1.conv_y.weight", 3) / std::max<int64_t>(1, ne("decoder.mid.block_1.norm1.conv_y.bias", 0)));
            const bool encoder     = std::all_of(config.encoder_channels.begin(), config.encoder_channels.end(), [](int v) { return v > 0; });
            const bool decoder     = std::all_of(config.decoder_channels.begin(), config.decoder_channels.end(), [](int v) { return v > 0; });
            if (!decoder || config.latent_channels != 64 || (encoder && ne("encoder.conv_out.conv.bias", 0) != 2 * config.latent_channels) ||
                ne("decoder.up.2.upsample.temporal_conv.conv.bias", 0) <= 0)
                throw std::runtime_error("unsupported Kandinsky 6 VSR K-VAE weights");
            LOG_VERBOSE("kandinsky6 k-vae: latent_channels = %d, decoder top width = %d%s", config.latent_channels,
                        config.decoder_channels.back(), encoder ? "" : " (decoder only)");
            return config;
        }
    };

    class KVAE : public GGMLBlock {
        KVAEConfig config;
        bool has_encoder;

    public:
        KVAE(const KVAEConfig& config, bool has_encoder)
            : config(config), has_encoder(has_encoder) {
            const int levels  = int(config.encoder_channels.size());
            const int z       = config.latent_channels;
            const int t_end   = config.time_start_level + config.time_levels;
            if (has_encoder) {
                const auto& ch       = config.encoder_channels;
                blocks["encoder.conv_in"] = std::make_shared<CausalConv>(3, ch[0], std::tuple<int, int, int>{3, 3, 3});
                for (int level = 0; level < levels; ++level) {
                    int in                  = level == 0 ? ch[0] : 2 * ch[level - 1];
                    const std::string stage = "encoder.down." + std::to_string(level) + ".";
                    for (int i = 0; i < config.layers; ++i) {
                        blocks[stage + "block." + std::to_string(i)] = std::make_shared<ResnetBlock>(in, ch[level]);
                        in                                           = ch[level];
                    }
                    if (level != levels - 1)
                        blocks[stage + "downsample"] = std::make_shared<Downsample>(in, config.time_start_level <= level && level < t_end);
                }
                blocks["encoder.mid.block_1"] = std::make_shared<ResnetBlock>(ch.back(), ch.back());
                blocks["encoder.mid.block_2"] = std::make_shared<ResnetBlock>(ch.back(), ch.back());
                blocks["encoder.norm_out"]    = std::make_shared<WAN::RMS_norm>(ch.back());
                blocks["encoder.conv_out"]    = std::make_shared<CausalConv>(ch.back(), 2 * z, std::tuple<int, int, int>{3, 3, 3});
            }
            const auto& ch                = config.decoder_channels;
            int in                        = ch.back();
            blocks["decoder.conv_in"]     = std::make_shared<CausalConv>(z, in, std::tuple<int, int, int>{3, 3, 3});
            blocks["decoder.mid.block_1"] = std::make_shared<ResnetBlock>(in, in, z);
            blocks["decoder.mid.block_2"] = std::make_shared<ResnetBlock>(in, in, z);
            for (int level = levels - 1; level >= 0; --level) {
                const std::string stage = "decoder.up." + std::to_string(level) + ".";
                for (int i = 0; i < config.layers + 1; ++i) {
                    blocks[stage + "block." + std::to_string(i)] = std::make_shared<ResnetBlock>(in, ch[level], z);
                    in                                           = ch[level];
                }
                if (level != 0)
                    blocks[stage + "upsample"] = std::make_shared<Upsample>(in, levels - config.time_start_level > level && level >= levels - t_end);
            }
            blocks["decoder.norm_out"] = std::make_shared<SpatialNorm>(in, z);
            blocks["decoder.conv_out"] = std::make_shared<CausalConv>(in, 3, std::tuple<int, int, int>{3, 3, 3});
        }

        template <typename T>
        std::shared_ptr<T> get(const std::string& name) { return std::dynamic_pointer_cast<T>(blocks[name]); }

        // pixels [W, H, T, 3] in [-1, 1] -> posterior mean [W/16, H/16, (T-1)/4+1, 64]
        ggml_tensor* encode(GGMLRunnerContext* ctx, ggml_tensor* x) {
            Causal state{ctx, false, true, 0};
            const int levels = int(config.encoder_channels.size());
            auto cut         = [&](const std::string& name) { sd::ggml_graph_cut::mark_graph_cut(x, "kvae.encoder." + name, "h"); };
            x                = get<CausalConv>("encoder.conv_in")->forward(state, x);
            for (int level = 0; level < levels; ++level) {
                const std::string stage = "encoder.down." + std::to_string(level) + ".";
                for (int i = 0; i < config.layers; ++i) {
                    x = get<ResnetBlock>(stage + "block." + std::to_string(i))->forward(state, x);
                    cut(stage + "block." + std::to_string(i));
                }
                if (level != levels - 1) {
                    x = get<Downsample>(stage + "downsample")->forward(state, x);
                    cut(stage + "downsample");
                }
            }
            x = get<ResnetBlock>("encoder.mid.block_1")->forward(state, x);
            x = get<ResnetBlock>("encoder.mid.block_2")->forward(state, x);
            x = ggml_silu(ctx->ggml_ctx, apply(ctx, blocks["encoder.norm_out"], x));
            x = get<CausalConv>("encoder.conv_out")->forward(state, x);
            return ggml_ext_slice(ctx, x, 3, 0, config.latent_channels);
        }

        // z [w, h, t, 64] (one segment) -> pixels in [-1, 1]
        ggml_tensor* decode(GGMLRunnerContext* ctx, ggml_tensor* z, bool first) {
            Causal state{ctx, true, first, 0};
            const int levels = int(config.decoder_channels.size());
            auto x           = get<CausalConv>("decoder.conv_in")->forward(state, z);
            auto cut         = [&](const std::string& name) { sd::ggml_graph_cut::mark_graph_cut(x, "kvae.decoder." + name, "h"); };
            x                = get<ResnetBlock>("decoder.mid.block_1")->forward(state, x, z);
            x                = get<ResnetBlock>("decoder.mid.block_2")->forward(state, x, z);
            cut("mid");
            for (int level = levels - 1; level >= 0; --level) {
                const std::string stage = "decoder.up." + std::to_string(level) + ".";
                for (int i = 0; i < config.layers + 1; ++i) {
                    x = get<ResnetBlock>(stage + "block." + std::to_string(i))->forward(state, x, z);
                    cut(stage + "block." + std::to_string(i));
                }
                if (level != 0) {
                    x = get<Upsample>(stage + "upsample")->forward(state, x);
                    cut(stage + "upsample");
                }
            }
            x = ggml_silu(ctx->ggml_ctx, get<SpatialNorm>("decoder.norm_out")->forward(state, x, z));
            return get<CausalConv>("decoder.conv_out")->forward(state, x);
        }
    };

    struct KVAERunner : public VAE {
        KVAEConfig config;
        bool has_encoder;
        KVAE model;

        KVAERunner(ggml_backend_t backend, const String2TensorStorage& weights, std::shared_ptr<RunnerWeightManager> manager,
                   SDVersion version, const std::string& prefix = "first_stage_model")
            : VAE(version, backend, prefix, manager),
              config(KVAEConfig::detect_from_weights(weights, prefix)),
              has_encoder(weights.count((prefix.empty() ? "" : prefix + ".") + "encoder.conv_out.conv.weight") > 0),
              model(config, has_encoder) {
            model.init(params_ctx, weights, prefix);
        }

        std::string get_desc() override { return "Kandinsky 6 VSR K-VAE"; }
        void get_param_tensors(std::map<std::string, ggml_tensor*>& tensors) override { model.get_param_tensors(tensors, weight_prefix); }
        int get_encoder_output_channels(int) override { return config.latent_channels; }
        // The encoder graph returns the posterior mean, so sampling and mode both use it.
        sd::Tensor<float> vae_output_to_latents(const sd::Tensor<float>& output, std::shared_ptr<RNG>, VAEPosterior, bool) override { return output; }
        sd::Tensor<float> vae_to_diffusion_latents(const sd::Tensor<float>& latent) override { return latent * SCALING_FACTOR; }
        sd::Tensor<float> diffusion_to_vae_latents(const sd::Tensor<float>& latent) override { return latent / SCALING_FACTOR; }

    protected:
        sd::Tensor<float> encode_volume(int n_threads, const sd::Tensor<float>& pixels) {
            if (!has_encoder)
                throw std::runtime_error("this K-VAE file has no encoder weights");
            if ((pixels.shape()[2] - 1) % 4 || pixels.shape()[0] % 16 || pixels.shape()[1] % 16)
                throw std::runtime_error("Kandinsky 6 VSR K-VAE encodes 4n+1 frames with sides divisible by 16");
            // The K-VAE was trained on pixels / 128 - 1, not pixels / 127.5 - 1.
            auto input = (pixels + 1.f) * (127.5f / 128.f) - 1.f;
            auto graph = [&]() {
                auto gf  = new_graph_custom(MAX_GRAPH_SIZE * 4);
                auto ctx = get_context(gf);
                ggml_build_forward_expand(gf, model.encode(&ctx, make_input(input)));
                return gf;
            };
            return restore_trailing_singleton_dims(GGMLRunner::compute(graph, n_threads), 4);
        }

        sd::Tensor<float> decode_volume(int n_threads, const sd::Tensor<float>& latent) {
            const int64_t w = latent.shape()[0], h = latent.shape()[1], t = latent.shape()[2], ch = latent.shape()[3];
            free_cache_ctx_and_buffer();
            sd::Tensor<float> output({w * 16, h * 16, (t - 1) * 4 + 1, 3});
            int64_t written = 0;
            for (int64_t i = 0; i < t; ++i) {
                sd::Tensor<float> segment({w, h, 1, ch});
                for (int64_t c = 0; c < ch; ++c)
                    std::copy_n(latent.data() + (c * t + i) * w * h, w * h, segment.data() + c * w * h);
                auto graph = [&]() {
                    auto gf  = new_graph_custom(MAX_GRAPH_SIZE * 4);
                    auto ctx = get_context(gf);
                    ggml_build_forward_expand(gf, model.decode(&ctx, make_input(segment), i == 0));
                    return gf;
                };
                auto frames = restore_trailing_singleton_dims(GGMLRunner::compute(graph, n_threads, false), 4);
                if (frames.empty()) {
                    free_cache_ctx_and_buffer();
                    return {};
                }
                const int64_t n = frames.shape()[2], plane = w * 16 * h * 16;
                for (int64_t c = 0; c < 3; ++c)
                    for (int64_t f = 0; f < n; ++f)
                        std::copy_n(frames.data() + (c * n + f) * plane, plane, output.data() + (c * output.shape()[2] + written + f) * plane);
                written += n;
            }
            free_cache_ctx_and_buffer();
            return (output + 1.f) * (128.f / 127.5f) - 1.f;
        }

        sd::Tensor<float> _compute(int n_threads, const sd::Tensor<float>& input, bool decode) override {
            if (input.empty() || (input.dim() != 4 && input.dim() != 5) || (input.dim() == 5 && input.shape().back() != 1) ||
                input.shape()[3] != (decode ? config.latent_channels : 3)) {
                throw std::runtime_error("Kandinsky 6 VSR K-VAE requires a single RGB video or 64-channel latent");
            }
            auto volume = input.dim() == 5 ? input.squeeze(4) : input;
            auto result = decode ? decode_volume(n_threads, volume) : encode_volume(n_threads, volume);
            if (result.empty())
                return {};
            return input.dim() == 5 ? result.unsqueeze(4) : result;
        }
    };
}  // namespace Kandinsky6KVAE

#endif  // __SD_MODEL_VAE_KANDINSKY6_KVAE_HPP__
