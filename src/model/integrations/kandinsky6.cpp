#include "model_definition.h"

#include "conditioning/conditioner.hpp"
#include "model_build_context.h"

static bool build_kandinsky6(const sd::model_builders::Context& ctx,
                             const TokenizerConfig& tokenizers,
                             sd::model_builders::CoreRunners& runners) {
    if (ctx.build_conditioner) {
        runners.conditioner = std::make_shared<LLMEmbedder>(ctx.backends.runtime_backend(SDBackendModule::TE),
                                                            ctx.tensor_storage_map, ctx.version, "", false,
                                                            ctx.weight_manager, tokenizers);
    }
    if (ctx.build_diffusion) {
        LOG_ERROR("Kandinsky 6 diffusion transformer is not supported yet");
        return false;
    }
    return true;
}

const std::vector<ModelDefinition>& kandinsky6_definitions() {
    static const std::vector<ModelDefinition> definitions = [] {
        ModelDefinition definition;
        definition.version                          = VERSION_KANDINSKY6;
        definition.geometry                         = {2, 16, 3};
        definition.name                             = "Kandinsky 6";
        definition.id                               = "kandinsky6";
        definition.traits.is_dit                    = true;
        definition.traits.supports_video_generation = true;
        definition.traits.video_tae                 = true;
        definition.sampling                         = {FLOW_PRED, 5.f};
        definition.build                            = build_kandinsky6;
        return std::vector<ModelDefinition>{std::move(definition)};
    }();
    return definitions;
}
