// Friendly names for the model folders under a model root.
export const categoryLabels: Record<string, string> = {
    diffusion_models: "Diffusion model",
    checkpoints: "Checkpoint (all-in-one)",
    text_encoders: "Text encoder",
    vae: "VAE / decoder",
    vae_approx: "Tiny preview decoder",
    clip_vision: "CLIP vision",
    tokenizers: "Tokenizer",
    loras: "LoRA",
    controlnet: "ControlNet",
    ipadapter: "IP-Adapter",
    motion_modules: "Motion module",
    photomaker: "PhotoMaker",
    pulid: "PuLID",
};

export function categoryLabel(category: string): string {
    return categoryLabels[category] || category;
}

export const categoryOrder = Object.keys(categoryLabels);
