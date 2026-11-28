#ifndef __SD_MODEL_DIFFUSION_KANDINSKY6_VSR_HPP__
#define __SD_MODEL_DIFFUSION_KANDINSKY6_VSR_HPP__

#include <numeric>

#include "model/diffusion/kandinsky6.hpp"

// Kandinsky 6.0 video super-resolution transformer (Kandinsky6SRTransformer3DModel in diffusers).
// A text-free, video-only variant of the Kandinsky 6 transformer: AdaLN self-attention blocks over
// K-VAE latent tiles, PiFlow grid output. The checkpoints were trained with NABLA sparse attention;
// this port runs full attention, which is identical whenever the NABLA sliding-tile prior covers
// every token block (tiles of at most 8 blocks per side and 6 latent frames).
namespace Kandinsky6VSR {
    namespace K6 = Kandinsky6;
    using K6::Kandinsky6Config;

    struct VSRConfig {
        Kandinsky6Config base;

        static VSRConfig detect_from_weights(const String2TensorStorage& weights, const std::string& prefix, float shift) {
            VSRConfig config;
            auto& c             = config.base;
            const std::string p = prefix.empty() ? "" : prefix + ".";
            auto ne             = [&](const std::string& name, int dim) -> int64_t {
                auto it = weights.find(p + name);
                return it == weights.end() ? -1 : it->second.ne[dim];
            };
            int blocks = 0;
            for (const auto& [name, _] : weights) {
                const std::string stem = p + "visual_transformer_blocks.";
                if (starts_with(name, stem))
                    blocks = std::max(blocks, atoi(name.c_str() + stem.size()) + 1);
            }
            c.num_visual_blocks = blocks;
            c.model_dim         = ne("visual_embeddings.in_layer.weight", 1);
            const int64_t in_ch = ne("visual_embeddings.in_layer.weight", 0);
            c.in_visual_dim     = (in_ch - 1) / 2;
            c.out_visual_dim    = ne("out_layer.out_layer.weight", 1);
            c.ff_dim            = ne("visual_transformer_blocks.0.feed_forward.net.0.proj.weight", 1);
            c.time_dim          = ne("time_embeddings.timestep_embedder.linear_2.weight", 1);
            c.head_dim          = ne("visual_transformer_blocks.0.self_attention.query_norm.weight", 0);
            c.patch_size        = {1, 1, 1};
            c.piflow_shift      = shift;
            if (blocks <= 0 || c.model_dim <= 0 || in_ch != 2 * c.in_visual_dim + 1 || c.head_dim <= 0 || c.head_dim % 16 ||
                c.model_dim % c.head_dim || c.out_visual_dim % c.in_visual_dim || c.ff_dim <= 0 || c.time_dim <= 0) {
                throw std::runtime_error("unsupported Kandinsky 6 VSR transformer weights");
            }
            c.axes_dims = {static_cast<int>(c.head_dim / 4), static_cast<int>(c.head_dim * 3 / 8), static_cast<int>(c.head_dim * 3 / 8)};
            LOG_INFO("kandinsky6 vsr: model_dim = %" PRId64 ", head_dim = %" PRId64 ", blocks = %d, latent channels = %" PRId64
                     ", n_grid = %" PRId64 ", shift = %.2f",
                     c.model_dim, c.head_dim, blocks, c.in_visual_dim, c.n_grid(), shift);
            return config;
        }
    };

    class SRBlock : public GGMLBlock {
        int64_t dim;
        float eps;

    public:
        SRBlock(const Kandinsky6Config& c)
            : dim(c.model_dim), eps(c.norm_eps) {
            blocks["visual_modulation"] = std::make_shared<K6::Modulation>(c.time_dim, 6 * dim);
            blocks["self_attention"]    = std::make_shared<K6::Attention>(dim, c.head_dim, dim, c.qk_norm_eps);
            blocks["feed_forward"]      = std::make_shared<K6::FeedForward>(dim, c.ff_dim);
        }

        ggml_tensor* forward(GGMLRunnerContext* ctx, ggml_tensor* x, ggml_tensor* t, ggml_tensor* pe) {
            auto modulation = std::dynamic_pointer_cast<K6::Modulation>(blocks["visual_modulation"]);
            auto attn       = std::dynamic_pointer_cast<K6::Attention>(blocks["self_attention"]);
            auto ff         = std::dynamic_pointer_cast<K6::FeedForward>(blocks["feed_forward"]);
            auto m          = modulation->forward(ctx, t);  // [sa shift, scale, gate | ff shift, scale, gate]

            auto h = K6::scale_shift(ctx, K6::plain_norm(ctx, x, eps), K6::param_chunk(ctx, m, dim, dim), K6::param_chunk(ctx, m, 0, dim));
            x      = K6::gate_sum(ctx, x, attn->forward(ctx, h, nullptr, pe, pe), K6::param_chunk(ctx, m, 2 * dim, dim));
            h      = K6::scale_shift(ctx, K6::plain_norm(ctx, x, eps), K6::param_chunk(ctx, m, 4 * dim, dim), K6::param_chunk(ctx, m, 3 * dim, dim));
            return K6::gate_sum(ctx, x, ff->forward(ctx, h), K6::param_chunk(ctx, m, 5 * dim, dim));
        }
    };

    class SRModel : public GGMLBlock {
        Kandinsky6Config c;

        void init_params(ggml_context* ctx, const String2TensorStorage&, const std::string) override {
            params["pooled_bias"] = ggml_new_tensor_1d(ctx, GGML_TYPE_F32, c.time_dim);
        }

    public:
        SRModel() = default;
        explicit SRModel(const Kandinsky6Config& config)
            : c(config) {
            blocks["time_embeddings"]            = std::make_shared<K6::TimeEmbeddings>(c.model_dim, c.time_dim);
            blocks["visual_embeddings.in_layer"] = std::make_shared<Linear>(c.visual_in_channels(), c.model_dim);
            blocks["out_layer"]                  = std::make_shared<K6::OutLayer>(c.model_dim, c.time_dim, c.out_visual_dim, c.norm_eps, false);
            for (int i = 0; i < c.num_visual_blocks; ++i)
                blocks["visual_transformer_blocks." + std::to_string(i)] = std::make_shared<SRBlock>(c);
        }

        // x: [W, H, T, 2C+1] -> [W, H, T, out_visual_dim]
        ggml_tensor* forward(GGMLRunnerContext* ctx, ggml_tensor* x, ggml_tensor* timesteps, ggml_tensor* pe) {
            auto g          = ctx->ggml_ctx;
            const int64_t W = x->ne[0], H = x->ne[1], T = x->ne[2], C = x->ne[3];
            auto time_layer = std::dynamic_pointer_cast<K6::TimeEmbeddings>(blocks["time_embeddings"]);
            auto temb       = ggml_add(g, time_layer->forward(ctx, timesteps), params["pooled_bias"]);

            auto tokens = ggml_cont(g, ggml_permute(g, ggml_reshape_2d(g, x, W * H * T, C), 1, 0, 2, 3));  // [C, L]
            auto in     = std::dynamic_pointer_cast<Linear>(blocks["visual_embeddings.in_layer"]);
            auto h      = in->forward(ctx, ggml_reshape_3d(g, tokens, C, W * H * T, 1));
            for (int i = 0; i < c.num_visual_blocks; ++i) {
                auto block = std::dynamic_pointer_cast<SRBlock>(blocks["visual_transformer_blocks." + std::to_string(i)]);
                h          = block->forward(ctx, h, temb, pe);
                sd::ggml_graph_cut::mark_graph_cut(h, "kandinsky6_vsr.blocks." + std::to_string(i), "h");
            }
            auto head = std::dynamic_pointer_cast<K6::OutLayer>(blocks["out_layer"]);
            h         = head->forward(ctx, h, temb);  // [out, L, 1]
            h         = ggml_reshape_2d(g, h, c.out_visual_dim, W * H * T);
            h         = ggml_cont(g, ggml_permute(g, h, 1, 0, 2, 3));  // [L, out]
            return ggml_reshape_4d(g, h, W, H, T, c.out_visual_dim);
        }
    };

    struct VSRRunner : public DiffusionModelRunner {
        VSRConfig config;
        SRModel model;
        sd::Tensor<float> input_, pe_;

        VSRRunner(ggml_backend_t backend,
                  const String2TensorStorage& weights,
                  const std::string& prefix,
                  std::shared_ptr<RunnerWeightManager> weight_manager,
                  float shift)
            : DiffusionModelRunner(backend, prefix, weight_manager),
              config(VSRConfig::detect_from_weights(weights, prefix, shift)),
              model(config.base) {
            model.init(params_ctx, weights, prefix);
        }

        std::string get_desc() override { return "kandinsky6_vsr"; }
        int get_patch_size() const override { return 1; }
        void get_param_tensors(std::map<std::string, ggml_tensor*>& tensors, const std::string& prefix) override {
            model.get_param_tensors(tensors, prefix);
        }

        // Raw model output [W, H, T, n_grid * C] for x [W, H, T, C] at timestep t (0..1000).
        sd::Tensor<float> forward_grid(int n_threads, const sd::Tensor<float>& x, float timestep) {
            const auto& c   = config.base;
            const int64_t W = x.shape()[0], H = x.shape()[1], T = x.shape()[2];
            // The released checkpoints are anchor-free: anchor latent and anchor mask channels are zero.
            input_ = sd::Tensor<float>::zeros({W, H, T, c.visual_in_channels()});
            std::copy_n(x.data(), static_cast<size_t>(W * H * T * c.in_visual_dim), input_.data());
            std::vector<int64_t> frames(static_cast<size_t>(T));
            std::iota(frames.begin(), frames.end(), int64_t(0));
            pe_ = K6::make_rope_3d(frames, H, W, c);
            auto timesteps = sd::Tensor<float>::from_vector({timestep});
            auto get_graph = [&]() -> ggml_cgraph* {
                ggml_cgraph* gf = new_graph_custom(K6::KANDINSKY6_GRAPH_SIZE);
                auto runner_ctx = get_context();
                auto out        = model.forward(&runner_ctx, make_input(input_), make_input(timesteps), make_input(pe_));
                ggml_build_forward_expand(gf, out);
                return gf;
            };
            auto out = GGMLRunner::compute(get_graph, n_threads, false);
            if (!out.has_value() || out->empty())
                return {};
            sd::Tensor<float> result({W, H, T, c.out_visual_dim});
            std::copy_n(out->data(), static_cast<size_t>(result.numel()), result.data());
            return result;
        }

        // The sampler hands one PiFlow segment per call; like Kandinsky 6 the returned flow velocity
        // (x - x_next) / (sigma - sigma_next) makes a plain Euler step land on the policy rollout.
        sd::Tensor<float> compute(int n_threads, const DiffusionParams& params) override {
            GGML_ASSERT(params.x != nullptr && params.timesteps != nullptr);
            const auto* extra = diffusion_extra_as<Kandinsky6DiffusionExtra>(params);
            if (!std::isfinite(extra->target_sigma))
                throw std::runtime_error("Kandinsky 6 VSR PiFlow sampling needs the step's target sigma; use the euler method");
            const auto& x  = *params.x;
            const auto& c  = config.base;
            const float t  = params.timesteps->data()[0];
            const float s  = t / 1000.f;
            const float s1 = extra->target_sigma;
            if (x.dim() < 4 || x.shape()[3] != c.in_visual_dim || s <= s1)
                throw std::runtime_error("Kandinsky 6 VSR latent must have " + std::to_string(c.in_visual_dim) + " channels");
            sd::Tensor<float> latent({x.shape()[0], x.shape()[1], x.shape()[2], c.in_visual_dim});
            const size_t n = static_cast<size_t>(latent.numel());
            std::copy_n(x.data(), n, latent.data());
            auto out = forward_grid(n_threads, latent, t);
            if (out.empty())
                return {};
            std::vector<const float*> grid;
            for (int64_t g = 0; g < c.n_grid(); ++g)
                grid.push_back(out.data() + g * static_cast<int64_t>(n));
            auto next = K6::piflow_rollout(latent.data(), grid, n, s, s1, c);
            sd::Tensor<float> result(x.shape());
            const float dt_inv = 1.f / (s - s1);
            for (size_t i = 0; i < n; ++i)
                result.data()[i] = (latent.data()[i] - next[i]) * dt_inv;
            return result;
        }
    };
}  // namespace Kandinsky6VSR

#endif  // __SD_MODEL_DIFFUSION_KANDINSKY6_VSR_HPP__
