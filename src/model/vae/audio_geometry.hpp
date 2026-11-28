#ifndef __SD_MODEL_VAE_AUDIO_GEOMETRY_HPP__
#define __SD_MODEL_VAE_AUDIO_GEOMETRY_HPP__

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <vector>

#include "core/image_geometry.h"
#include "model.h"

namespace sd::vae {
    inline std::vector<int64_t> audio_latent_shape(SDVersion version, int frames, int fps) {
        if (frames <= 0 || fps <= 0)
            throw std::invalid_argument("audio latent frame count and fps must be positive");
        if (sd_version_is_ltxav(version)) {
            constexpr float latents_per_second = 16000.0f / 160.0f / 4.0f;
            const auto length                  = static_cast<int64_t>(std::ceil((static_cast<float>(frames) / static_cast<float>(fps)) * latents_per_second));
            return {16, length, 8, 1};
        }
        if (sd_version_is_minimax_h3(version)) {
            const auto length = std::max<int64_t>(1, std::llround(static_cast<double>(frames) * 40.0 / fps));
            return {length, 2, 32, 1};
        }
        if (sd_version_is_kandinsky6(version)) {
            // MMAudio latents: 40 channels at 44.1 kHz / 1024 over the 4n+1-aligned frame count.
            const auto aligned = sd::align_video_frame_count(frames, 4);
            const auto length = static_cast<int64_t>(std::ceil(static_cast<double>(aligned) / fps * 44100.0 / 1024.0));
            return {length, 40, 1};
        }
        throw std::invalid_argument("audio latent shape is not defined for this model family");
    }
}

#endif
