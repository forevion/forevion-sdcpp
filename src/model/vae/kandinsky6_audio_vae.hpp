#ifndef __SD_MODEL_VAE_KANDINSKY6_AUDIO_VAE_HPP__
#define __SD_MODEL_VAE_KANDINSKY6_AUDIO_VAE_HPP__

#include <cmath>
#include <string>
#include <vector>

#include "model/vae/ltx_audio_vae.hpp"

// Kandinsky 6 audio decoder: the MMAudio 44.1 kHz mel VAE decoder followed by its BigVGAN-v2
// vocoder (diffusers MMAudioVAE + MMAudioVocoder). The released audio_vae checkpoint carries
// the vocoder weights under `vocoder.`; they are identical to the separate vocoder file.
namespace Kandinsky6Audio {
    // MMAudio's magnitude-preserving blocks clip activations to this range.
    constexpr float ACTIVATION_CLIP = 256.f;

    struct Config {
        int64_t mel_bins                = 128;
        int64_t latent_channels         = 40;
        int64_t hidden_channels         = 512;
        int layers_per_block            = 2;
        int sample_rate                 = 44100;
        float scaling_factor            = 0.417f;
        int64_t vocoder_initial_channel = 1536;
        std::vector<int> channel_multipliers{1, 2, 4};
        std::vector<int> upsample_rates{8, 4, 2, 2, 2, 2};
        std::vector<int> upsample_kernel_sizes{16, 8, 4, 4, 4, 4};
        std::vector<int> resblock_kernel_sizes{3, 7, 11};
        std::vector<std::vector<int>> resblock_dilation_sizes{{1, 3, 5}, {1, 3, 5}, {1, 3, 5}};
    };

    // x: [T, C] -> x / (eps + rms over C), the MMAudio `normalize(x, dim=1)`.
    static ggml_tensor* mp_normalize_channels(GGMLRunnerContext* ctx, ggml_tensor* x, float eps = 1e-4f) {
        auto c   = ctx->ggml_ctx;
        auto xt  = ggml_cont(c, ggml_transpose(c, x));  // [C, T]
        auto rms = ggml_sqrt(c, ggml_scale(c, ggml_sum_rows(c, ggml_sqr(c, xt)), 1.f / static_cast<float>(xt->ne[0])));
        rms      = ggml_scale_bias(c, rms, 1.f, eps);
        xt       = ggml_div(c, xt, rms);
        return ggml_cont(c, ggml_transpose(c, xt));
    }

    static ggml_tensor* mp_silu(GGMLRunnerContext* ctx, ggml_tensor* x) {
        return ggml_scale(ctx->ggml_ctx, ggml_silu(ctx->ggml_ctx, x), 1.f / 0.596f);
    }

    static ggml_tensor* mp_sum(GGMLRunnerContext* ctx, ggml_tensor* a, ggml_tensor* b, float t) {
        const float norm = std::sqrt((1.f - t) * (1.f - t) + t * t);
        auto c           = ctx->ggml_ctx;
        return ggml_add(c, ggml_scale(c, a, (1.f - t) / norm), ggml_scale(c, b, t / norm));
    }

    static ggml_tensor* clip_activation(GGMLRunnerContext* ctx, ggml_tensor* x) {
        return ggml_clamp(ctx->ggml_ctx, x, -ACTIVATION_CLIP, ACTIVATION_CLIP);
    }

    // ggml_conv_1d gathers its patches in F16; the BigVGAN vocoder amplifies that rounding to ~1%
    // of the waveform, so every convolution here keeps F32 patches.
    static std::shared_ptr<Conv1d> f32_conv(int64_t in, int64_t out, int kernel_size, int padding, int dilation = 1, bool bias = true) {
        return std::make_shared<Conv1d>(in, out, kernel_size, 1, padding, dilation, 1, bias, true);
    }

    static std::shared_ptr<Conv1d> mp_conv(int64_t in, int64_t out, int kernel_size) {
        return f32_conv(in, out, kernel_size, kernel_size / 2, 1, false);
    }

    // x: [T, C] filtered per channel by one shared [K, 1, 1] kernel (channels ride in the batch axis).
    static ggml_tensor* shared_filter_1d(GGMLRunnerContext* ctx, ggml_tensor* x, ggml_tensor* filter, int stride, int padding) {
        auto c   = ctx->ggml_ctx;
        auto out = ggml_ext_conv_1d(ctx, ggml_reshape_3d(c, x, x->ne[0], 1, x->ne[1]), filter, nullptr, stride, padding, 1, 1, true);
        return ggml_reshape_2d(c, out, out->ne[0], x->ne[1]);
    }

    // alias-free-torch Activation1d: 2x Kaiser-sinc upsample, Snake-beta, 2x low-pass downsample.
    struct Activation : public GGMLBlock {
        static constexpr int ratio       = 2;
        static constexpr int kernel_size = 12;

        explicit Activation(int64_t channels) {
            blocks["act"] = std::make_shared<LTXV::SnakeBeta1D>(channels);
        }

        void init_params(ggml_context* ctx, const String2TensorStorage& tensor_storage_map = {}, const std::string prefix = "") override {
            SD_UNUSED(tensor_storage_map);
            SD_UNUSED(prefix);
            params["upsample.filter"]           = ggml_new_tensor_3d(ctx, GGML_TYPE_F32, kernel_size, 1, 1);
            params["downsample.lowpass.filter"] = ggml_new_tensor_3d(ctx, GGML_TYPE_F32, kernel_size, 1, 1);
        }

        ggml_tensor* forward(GGMLRunnerContext* ctx, ggml_tensor* x) {
            auto c          = ctx->ggml_ctx;
            const int pad   = kernel_size / ratio - 1;
            const int left  = pad * ratio + (kernel_size - ratio) / 2;
            const int right = pad * ratio + (kernel_size - ratio + 1) / 2;
            x               = ggml_reshape_2d(c, x, x->ne[0], x->ne[1]);
            x               = LTXV::replicate_pad_1d(ctx, x, pad, pad);

            // conv_transpose1d(stride 2) == zero-stuffing followed by a full convolution with the reversed kernel.
            const int64_t T      = x->ne[0], C = x->ne[1];
            const int64_t up_len = (T - 1) * ratio + kernel_size;
            auto x3              = ggml_reshape_3d(c, x, 1, T, C);
            auto stuffed         = ggml_reshape_2d(c, ggml_concat(c, x3, ggml_scale(c, x3, 0.f), 0), ratio * T, C);
            auto up              = shared_filter_1d(ctx, stuffed, LTXV::reverse_1d_filter(ctx, params["upsample.filter"]), 1, kernel_size - 1);
            up                   = ggml_scale(c, ggml_ext_slice(ctx, up, 0, left, up_len - right), static_cast<float>(ratio));

            up = std::dynamic_pointer_cast<LTXV::SnakeBeta1D>(blocks["act"])->forward(ctx, up);
            up = ggml_reshape_2d(c, up, up->ne[0], up->ne[1]);
            up = LTXV::replicate_pad_1d(ctx, up, kernel_size / 2 - 1, kernel_size / 2);
            return shared_filter_1d(ctx, up, params["downsample.lowpass.filter"], ratio, 0);
        }
    };

    struct ResnetBlock : public GGMLBlock {
        int64_t in_channels, out_channels;

        ResnetBlock(int64_t in_channels, int64_t out_channels)
            : in_channels(in_channels), out_channels(out_channels) {
            blocks["conv1"] = mp_conv(in_channels, out_channels, 3);
            blocks["conv2"] = mp_conv(out_channels, out_channels, 3);
            if (in_channels != out_channels) {
                blocks["nin_shortcut"] = mp_conv(in_channels, out_channels, 1);
            }
        }

        ggml_tensor* forward(GGMLRunnerContext* ctx, ggml_tensor* x) {
            auto conv1 = std::dynamic_pointer_cast<Conv1d>(blocks["conv1"]);
            auto conv2 = std::dynamic_pointer_cast<Conv1d>(blocks["conv2"]);
            x          = mp_normalize_channels(ctx, x);
            auto h     = conv1->forward(ctx, mp_silu(ctx, x));
            h          = conv2->forward(ctx, mp_silu(ctx, h));
            if (in_channels != out_channels) {
                x = std::dynamic_pointer_cast<Conv1d>(blocks["nin_shortcut"])->forward(ctx, x);
            }
            return mp_sum(ctx, x, h, 0.3f);
        }
    };

    // Single-head attention over the full channel width; q, k and v are interleaved in the qkv
    // projection's channels (index = 3 * c + {0, 1, 2}).
    struct AttnBlock : public GGMLBlock {
        int64_t channels;

        explicit AttnBlock(int64_t channels)
            : channels(channels) {
            blocks["qkv"]      = mp_conv(channels, channels * 3, 1);
            blocks["proj_out"] = mp_conv(channels, channels, 1);
        }

        ggml_tensor* forward(GGMLRunnerContext* ctx, ggml_tensor* x) {
            auto c          = ctx->ggml_ctx;
            const int64_t T = x->ne[0];
            auto qkv        = std::dynamic_pointer_cast<Conv1d>(blocks["qkv"])->forward(ctx, x);  // [T, 3C]
            qkv             = ggml_cont(c, ggml_transpose(c, ggml_reshape_2d(c, qkv, T, 3 * channels)));  // [3C, T]
            ggml_tensor* parts[3];
            for (int j = 0; j < 3; ++j) {
                auto view = ggml_view_2d(c, qkv, 1, channels * T, 3 * ggml_element_size(qkv), j * ggml_element_size(qkv));
                auto part = ggml_reshape_2d(c, ggml_cont(c, view), channels, T);  // [C, T]
                auto norm = ggml_sqrt(c, ggml_scale(c, ggml_sum_rows(c, ggml_sqr(c, part)), 1.f / static_cast<float>(channels)));
                parts[j]  = ggml_div(c, part, ggml_scale_bias(c, norm, 1.f, 1e-4f));
            }
            auto scores = ggml_mul_mat(c, parts[1], parts[0]);  // [T_k, T_q]
            scores      = ggml_soft_max_ext(c, scores, nullptr, 1.f / std::sqrt(static_cast<float>(channels)), 0.f);
            auto v_t    = ggml_cont(c, ggml_transpose(c, parts[2]));  // [T_k, C]
            auto out    = ggml_mul_mat(c, v_t, scores);                // [C, T_q]
            out         = ggml_cont(c, ggml_transpose(c, out));        // [T, C]
            out         = std::dynamic_pointer_cast<Conv1d>(blocks["proj_out"])->forward(ctx, out);
            return mp_sum(ctx, x, ggml_reshape_2d(c, out, T, channels), 0.3f);
        }
    };

    struct Decoder : public GGMLBlock {
        Config config;

        explicit Decoder(const Config& config)
            : config(config) {
            const auto& mult      = config.channel_multipliers;
            int64_t block_in      = config.hidden_channels * mult.back();
            blocks["conv_in"]     = mp_conv(config.latent_channels, block_in, 3);
            blocks["mid.block_1"] = std::make_shared<ResnetBlock>(block_in, block_in);
            blocks["mid.attn_1"]  = std::make_shared<AttnBlock>(block_in);
            blocks["mid.block_2"] = std::make_shared<ResnetBlock>(block_in, block_in);
            for (int level = static_cast<int>(mult.size()) - 1; level >= 0; --level) {
                const int64_t block_out = config.hidden_channels * mult[level];
                for (int i = 0; i < config.layers_per_block + 1; ++i) {
                    blocks["up." + std::to_string(level) + ".block." + std::to_string(i)] = std::make_shared<ResnetBlock>(block_in, block_out);
                    block_in                                                               = block_out;
                }
                if (level == 1) {
                    blocks["up.1.upsample.conv"] = mp_conv(block_in, block_in, 3);
                }
            }
            blocks["conv_out"] = mp_conv(block_in, config.mel_bins, 3);
        }

        void init_params(ggml_context* ctx, const String2TensorStorage& tensor_storage_map = {}, const std::string prefix = "") override {
            SD_UNUSED(tensor_storage_map);
            SD_UNUSED(prefix);
            params["learnable_gain"] = ggml_new_tensor_1d(ctx, GGML_TYPE_F32, 1);
        }

        ggml_tensor* forward(GGMLRunnerContext* ctx, ggml_tensor* z) {
            auto c   = ctx->ggml_ctx;
            auto res = [&](const std::string& name, ggml_tensor* x) {
                return std::dynamic_pointer_cast<ResnetBlock>(blocks[name])->forward(ctx, x);
            };
            auto conv = [&](const std::string& name, ggml_tensor* x) {
                return std::dynamic_pointer_cast<Conv1d>(blocks[name])->forward(ctx, x);
            };
            auto h           = conv("conv_in", z);
            h                = res("mid.block_1", h);
            h                = std::dynamic_pointer_cast<AttnBlock>(blocks["mid.attn_1"])->forward(ctx, h);
            h                = clip_activation(ctx, res("mid.block_2", h));
            const auto& mult = config.channel_multipliers;
            for (int level = static_cast<int>(mult.size()) - 1; level >= 0; --level) {
                for (int i = 0; i < config.layers_per_block + 1; ++i) {
                    h = clip_activation(ctx, res("up." + std::to_string(level) + ".block." + std::to_string(i), h));
                }
                if (level == 1) {
                    // nearest-exact 2x: every frame is repeated once.
                    const int64_t T = h->ne[0], C = h->ne[1];
                    auto x3         = ggml_reshape_3d(c, h, 1, T, C);
                    h               = ggml_reshape_2d(c, ggml_concat(c, x3, x3, 0), 2 * T, C);
                    h               = conv("up.1.upsample.conv", h);
                }
            }
            h         = conv("conv_out", mp_silu(ctx, h));
            auto gain = ggml_scale_bias(c, params["learnable_gain"], 1.f, 1.f);
            return ggml_mul(c, h, gain);
        }
    };

    struct AMPBlock : public GGMLBlock {
        std::vector<int> dilations;

        AMPBlock(int64_t channels, int kernel_size, const std::vector<int>& dilations)
            : dilations(dilations) {
            for (size_t i = 0; i < dilations.size(); ++i) {
                const int d                                        = dilations[i];
                blocks["convs1." + std::to_string(i)]             = f32_conv(channels, channels, kernel_size, (kernel_size * d - d) / 2, d);
                blocks["convs2." + std::to_string(i)]             = f32_conv(channels, channels, kernel_size, (kernel_size - 1) / 2);
                blocks["activations." + std::to_string(2 * i)]     = std::make_shared<Activation>(channels);
                blocks["activations." + std::to_string(2 * i + 1)] = std::make_shared<Activation>(channels);
            }
        }

        ggml_tensor* forward(GGMLRunnerContext* ctx, ggml_tensor* x) {
            for (size_t i = 0; i < dilations.size(); ++i) {
                auto act1  = std::dynamic_pointer_cast<Activation>(blocks["activations." + std::to_string(2 * i)]);
                auto act2  = std::dynamic_pointer_cast<Activation>(blocks["activations." + std::to_string(2 * i + 1)]);
                auto conv1 = std::dynamic_pointer_cast<Conv1d>(blocks["convs1." + std::to_string(i)]);
                auto conv2 = std::dynamic_pointer_cast<Conv1d>(blocks["convs2." + std::to_string(i)]);
                auto h     = conv2->forward(ctx, act2->forward(ctx, conv1->forward(ctx, act1->forward(ctx, x))));
                x          = ggml_add(ctx->ggml_ctx, x, h);
            }
            return x;
        }
    };

    struct Vocoder : public GGMLBlock {
        Config config;

        explicit Vocoder(const Config& config)
            : config(config) {
            blocks["conv_pre"] = f32_conv(config.mel_bins, config.vocoder_initial_channel, 7, 3);
            int64_t channels   = config.vocoder_initial_channel;
            int resblock       = 0;
            for (size_t i = 0; i < config.upsample_rates.size(); ++i) {
                const int rate = config.upsample_rates[i], kernel = config.upsample_kernel_sizes[i];
                blocks["ups." + std::to_string(i) + ".0"] = std::make_shared<LTXV::ConvTranspose1D>(channels, channels / 2, kernel, rate, (kernel - rate) / 2);
                channels /= 2;
                for (size_t j = 0; j < config.resblock_kernel_sizes.size(); ++j) {
                    blocks["resblocks." + std::to_string(resblock++)] =
                        std::make_shared<AMPBlock>(channels, config.resblock_kernel_sizes[j], config.resblock_dilation_sizes[j]);
                }
            }
            blocks["activation_post"] = std::make_shared<Activation>(channels);
            blocks["conv_post"]       = f32_conv(channels, 1, 7, 3, 1, false);
        }

        ggml_tensor* forward(GGMLRunnerContext* ctx, ggml_tensor* mel) {
            auto x             = std::dynamic_pointer_cast<Conv1d>(blocks["conv_pre"])->forward(ctx, mel);
            const size_t kinds = config.resblock_kernel_sizes.size();
            for (size_t i = 0; i < config.upsample_rates.size(); ++i) {
                x                = std::dynamic_pointer_cast<LTXV::ConvTranspose1D>(blocks["ups." + std::to_string(i) + ".0"])->forward(ctx, x);
                ggml_tensor* sum = nullptr;
                for (size_t j = 0; j < kinds; ++j) {
                    auto out = std::dynamic_pointer_cast<AMPBlock>(blocks["resblocks." + std::to_string(i * kinds + j)])->forward(ctx, x);
                    sum      = sum == nullptr ? out : ggml_add(ctx->ggml_ctx, sum, out);
                }
                x = ggml_scale(ctx->ggml_ctx, sum, 1.f / static_cast<float>(kinds));
            }
            x = std::dynamic_pointer_cast<Activation>(blocks["activation_post"])->forward(ctx, x);
            x = std::dynamic_pointer_cast<Conv1d>(blocks["conv_post"])->forward(ctx, x);
            return ggml_clamp(ctx->ggml_ctx, x, -1.f, 1.f);
        }
    };

    struct AudioVAE : public GGMLBlock {
        Config config;
        std::string vae_prefix;

        AudioVAE(const Config& config, const std::string& vae_prefix)
            : config(config), vae_prefix(vae_prefix) {
            blocks[vae_prefix + "decoder"] = std::make_shared<Decoder>(config);
            blocks["vocoder"]              = std::make_shared<Vocoder>(config);
        }

        void init_params(ggml_context* ctx, const String2TensorStorage& tensor_storage_map = {}, const std::string prefix = "") override {
            SD_UNUSED(tensor_storage_map);
            SD_UNUSED(prefix);
            params[vae_prefix + "data_mean"] = ggml_new_tensor_3d(ctx, GGML_TYPE_F32, 1, config.mel_bins, 1);
            params[vae_prefix + "data_std"]  = ggml_new_tensor_3d(ctx, GGML_TYPE_F32, 1, config.mel_bins, 1);
        }

        // latent: [L, latent_channels] (as the sampler packs it, not yet divided by scaling_factor).
        // Returns the waveform [1024 L, 1] in [-1, 1].
        ggml_tensor* decode(GGMLRunnerContext* ctx, ggml_tensor* latent) {
            auto c    = ctx->ggml_ctx;
            auto z    = ggml_scale(c, ggml_reshape_2d(c, latent, latent->ne[0], latent->ne[1]), 1.f / config.scaling_factor);
            auto mel  = std::dynamic_pointer_cast<Decoder>(blocks[vae_prefix + "decoder"])->forward(ctx, z);
            auto sdev = ggml_reshape_2d(c, params[vae_prefix + "data_std"], 1, config.mel_bins);
            auto mu   = ggml_reshape_2d(c, params[vae_prefix + "data_mean"], 1, config.mel_bins);
            mel       = ggml_add(c, ggml_mul(c, mel, sdev), mu);
            return std::dynamic_pointer_cast<Vocoder>(blocks["vocoder"])->forward(ctx, mel);
        }
    };

    static std::string detect_vae_prefix(const String2TensorStorage& tensor_storage_map, const std::string& prefix) {
        const std::string p = prefix.empty() ? "" : prefix + ".";
        for (const char* candidate : {"first_stage_model.", "vae.", ""}) {
            if (tensor_storage_map.count(p + candidate + "decoder.conv_in.weight") &&
                tensor_storage_map.count(p + candidate + "data_std")) {
                return candidate;
            }
        }
        return "first_stage_model.";
    }

    static bool is_kandinsky6_audio_vae(const String2TensorStorage& tensor_storage_map, const std::string& prefix = "") {
        const std::string p = prefix.empty() ? "" : prefix + ".";
        return tensor_storage_map.count(p + detect_vae_prefix(tensor_storage_map, prefix) + "decoder.mid.attn_1.qkv.weight") &&
               tensor_storage_map.count(p + "vocoder.conv_pre.weight");
    }

    struct AudioVAERunner : public ::AudioVAERunner {
        Config config;
        std::string vae_prefix;
        AudioVAE model;
        std::string weight_prefix;

        AudioVAERunner(ggml_backend_t backend,
                       const String2TensorStorage& tensor_storage_map,
                       const std::string& prefix                           = "",
                       std::shared_ptr<RunnerWeightManager> weight_manager = nullptr)
            : ::AudioVAERunner(backend, weight_manager),
              vae_prefix(detect_vae_prefix(tensor_storage_map, prefix)),
              model(config, vae_prefix),
              weight_prefix(prefix) {
            if (!is_kandinsky6_audio_vae(tensor_storage_map, prefix)) {
                throw std::runtime_error("Kandinsky 6 needs the MMAudio audio_vae checkpoint (decoder and vocoder weights)");
            }
            model.init(params_ctx, tensor_storage_map, prefix);
        }

        void get_param_tensors(std::map<std::string, ggml_tensor*>& tensors) override {
            model.get_param_tensors(tensors, weight_prefix);
        }

        size_t get_params_mem_size() override {
            return model.get_params_mem_size();
        }

        std::string get_desc() override {
            return "kandinsky6_audio_vae";
        }

        sd::Tensor<float> decode(int n_threads, const sd::Tensor<float>& latent_tensor) override {
            const int64_t t0 = ggml_time_ms();
            auto get_graph   = [&]() -> ggml_cgraph* {
                auto latent     = make_input(latent_tensor);
                ggml_cgraph* gf = new_graph_custom(65536);
                auto runner_ctx = get_context();
                ggml_build_forward_expand(gf, model.decode(&runner_ctx, latent));
                return gf;
            };
            auto result = restore_trailing_singleton_dims(GGMLRunner::compute(get_graph, n_threads, false), 4);
            LOG_INFO("kandinsky6 audio vae decode completed, taking %.2fs", (ggml_time_ms() - t0) / 1000.f);
            return result;
        }

        int output_sample_rate() const override {
            return config.sample_rate;
        }
    };
}  // namespace Kandinsky6Audio

#endif  // __SD_MODEL_VAE_KANDINSKY6_AUDIO_VAE_HPP__
