#include "model/diffusion/kandinsky6_vsr.hpp"
#include "model_build_context.h"
#include "model_definition.h"

// The VSR checkpoints share one architecture; their PiFlow flow shift (scheduler_scale) is only in
// the scheduler config, so it is selected by family.
static constexpr float KANDINSKY6_VSR_SHIFT    = 5.f;
static constexpr float KANDINSKY6_VSR_5S_SHIFT = 3.5f;

static bool build_kandinsky6_vsr(const sd::model_builders::Context& ctx, const TokenizerConfig&, sd::model_builders::CoreRunners& runners) {
    if (!ctx.build_diffusion || ctx.build_conditioner)
        return false;
    const float shift = ctx.version == VERSION_KANDINSKY6_VSR_5S ? KANDINSKY6_VSR_5S_SHIFT : KANDINSKY6_VSR_SHIFT;
    runners.diffusion = std::make_shared<Kandinsky6VSR::VSRRunner>(ctx.backends.runtime_backend(SDBackendModule::DIFFUSION),
                                                                   ctx.tensor_storage_map, "model.diffusion_model", ctx.weight_manager, shift);
    return true;
}

const std::vector<ModelDefinition>& kandinsky6_vsr_definitions() {
    static const std::vector<ModelDefinition> definitions = [] {
        ModelDefinition definition;
        definition.version                          = VERSION_KANDINSKY6_VSR;
        definition.name                             = "Kandinsky 6 VSR";
        definition.id                               = "kandinsky6_vsr";
        definition.geometry                         = {1, 64, 3};
        definition.traits.is_dit                    = true;
        definition.traits.supports_video_generation = true;
        definition.sampling                         = {FLOW_PRED, KANDINSKY6_VSR_SHIFT};
        definition.sampling.default_sample_method   = EULER_SAMPLE_METHOD;
        definition.sampling.default_scheduler       = PIFLOW_SCHEDULER;
        definition.build                            = build_kandinsky6_vsr;

        ModelDefinition five_seconds             = definition;
        five_seconds.version                     = VERSION_KANDINSKY6_VSR_5S;
        five_seconds.name                        = "Kandinsky 6 VSR (5s)";
        five_seconds.id                          = "kandinsky6_vsr_5s";
        five_seconds.sampling.default_flow_shift = KANDINSKY6_VSR_5S_SHIFT;
        return std::vector<ModelDefinition>{std::move(definition), std::move(five_seconds)};
    }();
    return definitions;
}
