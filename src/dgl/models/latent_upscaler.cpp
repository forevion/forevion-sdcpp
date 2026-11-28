#include "dgl/models/latent_upscaler.h"

#include "core/image_geometry.h"
#include "dgl/models/component_sources.h"
#include "model/upscaler/h3_latent_upscaler.hpp"
#include "model/upscaler/kandinsky6_latent_upscaler.hpp"
#include "model/upscaler/ltx_latent_upscaler.hpp"

namespace sd::dgl::models {
    LatentUpscaler::LatentUpscaler(std::shared_ptr<ModelManager> manager, std::shared_ptr<SDBackendPool> backends)
        : Model(std::move(manager)),
          runtime_(*this, std::move(backends), ModelComponent::LatentUpsampler, SDBackendModule::DIFFUSION, [this] { return build_runners(); }) {}

    bool LatentUpscaler::init(ModelLoadParams params) {
        return runtime_.init(std::move(params), load_component_catalog<ModelComponent::LatentUpsampler>);
    }

    bool LatentUpscaler::build_variant(const Model& source, ModelLoader loader) {
        return runtime_.build_variant(static_cast<const LatentUpscaler&>(source).runtime_, std::move(loader));
    }

    bool LatentUpscaler::build_runners() {
        const auto& weights     = model_weights->loader().get_tensor_storage_map();
        const auto backend      = runtime_.backend_for();
        const auto linear_scale = runtime_.params.linear_scale;
        const auto attn_scale   = runtime_.params.attn_scale;
        if (sd_version_is_kandinsky6_vsr(runtime_.version)) {
            // model_args "scale=2" or "scale=4" loads one model of the bank to halve its memory.
            int only_scale = 0;
            for (const auto& [key, value] : parse_key_value_args(runtime_.params.model_args.c_str(), "model arg")) {
                if (key != "scale")
                    continue;
                if (value != "2" && value != "4")
                    throw std::runtime_error("Kandinsky 6 latent upscaler model_args take scale=2 or scale=4");
                only_scale = std::stoi(value);
            }
            k6_ = std::make_shared<Kandinsky6Upscaler::UpscalerRunner>(backend, weights, "k6_latent_upscaler", model_weights, only_scale);
            k6_->set_scale_overrides(linear_scale, attn_scale);
            return runtime_.register_runner_params(runtime_.component, k6_);
        }
        if (sd_version_is_ltxav(runtime_.version)) {
            ltx_ = std::make_shared<LTXVUpsampler::LatentUpsamplerRunner>(backend, weights, "ltx_latent_upsampler", model_weights);
            if (!ltx_->model)
                return false;
            ltx_->set_conv2d_direct_enabled(runtime_.conv_direct_for(false));
            ltx_->set_scale_overrides(linear_scale, attn_scale);
            return runtime_.register_runner_params(runtime_.component, ltx_);
        }

        h3_ = std::make_shared<H3VUpsampler::H3LatentUpscalerRunner>(backend, weights, "h3_latent_upscaler", model_weights);
        if (!h3_->model || h3_->config.in_channels != 24)
            return false;
        h3_->set_scale_overrides(linear_scale, attn_scale);
        return runtime_.register_runner_params(runtime_.component, h3_);
    }

    Tensor<float> LatentUpscaler::upscale(const Tensor<float>& latent, int64_t width, int64_t height) {
        if (k6_) {
            if (latent.empty() || latent.dim() < 4 || latent.shape()[3] != k6_->config.in_channels)
                throw std::runtime_error("Kandinsky 6 latent upscaling requires a [W,H,T,64] K-VAE latent");
            const int64_t scale = width ? width / latent.shape()[0] : 2;
            if ((width && width != scale * latent.shape()[0]) || (height && height != scale * latent.shape()[1]))
                throw std::runtime_error("Kandinsky 6 latent upscaler targets must be 2x or 4x the latent size on both axes");
            return k6_->compute(n_threads, latent, int(scale));
        }
        const auto channels = ltx_ ? ltx_->config.in_channels : h3_->config.in_channels;
        const auto rank     = latent.dim();
        if (latent.empty() || (rank != 4 && rank != 5) ||
            (rank == 5 && latent.shape()[4] != 1) || latent.shape()[3] != channels) {
            throw std::runtime_error("latent upscaling requires [W,H,T,C] or [W,H,T,C,1] with " + std::to_string(channels) + " channels");
        }
        if (ltx_) {
            if (width || height)
                throw std::runtime_error("LTX latent upscaler determines its scale from the checkpoint");
            return ltx_->compute(n_threads, latent);
        }

        const auto aligned_width  = sd::align_image_dimension(width, 2);
        const auto aligned_height = sd::align_image_dimension(height, 2);
        if (width != aligned_width || height != aligned_height)
            LOG_VERBOSE("upscale_latent: aligning target from %lldx%lld to %lldx%lld", (long long)width, (long long)height,
                        (long long)aligned_width, (long long)aligned_height);
        width  = aligned_width;
        height = aligned_height;
        if (width == latent.shape()[0] && height == latent.shape()[1])
            return latent;
        return h3_->compute(n_threads, latent, width, height);
    }
}
