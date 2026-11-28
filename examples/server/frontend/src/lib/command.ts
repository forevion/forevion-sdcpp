import { buildRequest, currentLoras, currentRecipe, inputSupport, loraEntries, loraInstalled, plan, recipeIsVideo, selectedFiles, state } from "./store";

// sd-cli flag for each model input (slot) of a config.
const SLOT_FLAGS: Record<string, string> = {
    model: "--model",
    diffusion_model: "--diffusion-model",
    high_noise_diffusion_model: "--high-noise-diffusion-model",
    uncond_diffusion_model: "--uncond-diffusion-model",
    clip_l: "--clip_l",
    clip_g: "--clip_g",
    clip_vision: "--clip_vision",
    t5xxl: "--t5xxl",
    llm: "--llm",
    llm_vision: "--llm_vision",
    tokenizer: "--tokenizer",
    vae: "--vae",
    tae: "--tae",
    taesd: "--tae",
    audio_vae: "--audio-vae",
    audio_encoder: "--audio-encoder",
    latent_upscaler: "--latent-upscaler",
    embeddings_connectors: "--embeddings-connectors",
    control_net: "--control-net",
};

export interface CommandPreview {
    windows: boolean;
    cli: string;
    notes: string[];
    endpoint: string;
    requestJson: string;
    curl: string;
}

function isWindowsRoot(root: string) {
    return /^[A-Za-z]:[\\/]/.test(root);
}

// One shell argument, quoted for PowerShell (Windows) or POSIX shells.
function quote(value: string, windows: boolean) {
    if (/^[A-Za-z0-9_./:=,+@-]+$/.test(value)) return value;
    if (windows) return `"${value.replace(/`/g, "``").replace(/"/g, '`"').replace(/\$/g, "`$")}"`;
    return `'${value.replace(/'/g, `'\\''`)}'`;
}

function modelPath(root: string, id: string, windows: boolean) {
    if (id.startsWith("@")) return id;
    const separator = windows ? "\\" : "/";
    return root.replace(/[\\/]+$/, "") + separator + id.split("/").join(separator);
}

export function commandPreview(): CommandPreview | null {
    const recipe = currentRecipe.value;
    const request = buildRequest();
    if (!recipe) return null;
    const p = plan.value;
    const form = state.form;
    const root = state.downloads?.model_root || "models";
    const windows = isWindowsRoot(root);
    const defaults = recipe.defaults || {};
    const notes: string[] = [];
    const args: string[][] = [];
    const add = (flag: string, value?: string | number) => args.push(value === undefined ? [flag] : [flag, String(value)]);

    const video = recipeIsVideo(recipe);
    add("--mode", video ? "vid_gen" : "img_gen");
    for (const [slot, file] of Object.entries(selectedFiles.value)) {
        if (!file) continue;
        const flag = SLOT_FLAGS[slot];
        if (flag) add(flag, modelPath(root, file, windows));
        else notes.push(`The ${slot} input has no sd-cli flag; it is only available through the server API.`);
    }
    const context = (recipe as unknown as { context?: Record<string, string> }).context || {};
    if (context.vae_format) add("--vae-format", context.vae_format);
    if (context.model_args) add("--model-args", context.model_args);

    // sd-cli reads LoRAs from <lora:name:strength> tags in the prompt, relative to --lora-model-dir.
    const loraTags = loraEntries.value.map((l) => `<lora:${l.is_high_noise ? "|high_noise|" : ""}${l.path}:${l.multiplier}>`);
    if (loraTags.length) add("--lora-model-dir", modelPath(root, "loras", windows));
    if (currentLoras.value.some((l) => !loraInstalled(l.path))) notes.push("LoRAs whose files are missing are left out.");
    add("--prompt", [form.prompt.trim() || "your prompt", ...loraTags].join(" "));
    if (form.negativePrompt.trim()) add("--negative-prompt", form.negativePrompt.trim());
    add("--width", p.width);
    add("--height", p.height);
    if (video) {
        add("--video-frames", p.renderFrames);
        add("--fps", p.fps);
    }
    const steps = form.steps ?? defaults.steps;
    if (steps != null) add("--steps", steps);
    const cfg = form.cfg ?? defaults.cfg_scale;
    if (cfg != null) add("--cfg-scale", cfg);
    if (defaults.sampling_method) add("--sampling-method", defaults.sampling_method);
    if (defaults.scheduler) add("--scheduler", defaults.scheduler);
    if (defaults.flow_shift != null) add("--flow-shift", defaults.flow_shift);
    if (defaults.custom_sigmas?.length && form.steps == null) add("--sigmas", defaults.custom_sigmas.join(","));
    add("--seed", form.lockSeed ? form.seed : -1);

    const support = inputSupport.value;
    const inputs = state.inputs;
    let usesFiles = false;
    if (support.startFrame && inputs.startImage) (add("--init-img", inputs.startName || "start-frame.png"), (usesFiles = true));
    if (support.endFrame && inputs.endImage) (add("--end-img", inputs.endName || "end-frame.png"), (usesFiles = true));
    if (support.refImages) for (const image of inputs.refImages) (add("--ref-image", image.name), (usesFiles = true));
    if (support.audio && inputs.audio) (add("--audio", inputs.audio.name), (usesFiles = true));
    if (support.control && inputs.control) (add("--control-video", inputs.control.name), (usesFiles = true));
    if (support.strength) add("--strength", form.strength);
    if (usesFiles) notes.push("Input files are named as you picked them; use their full paths on the command line.");
    if (support.sound) notes.push("This model makes sound: use an .avi output to keep it (WebM is silent).");

    add("--mmap");
    add("--auto-fit", "off");
    add("--vae-tiling");
    const output = video && p.renderFrames > 1 ? (support.sound ? "output.avi" : "output.webm") : "output.png";
    add("--output", output);
    if (p.still) notes.push(`The server renders ${p.renderFrames} frames and keeps frame ${p.still.keep + 1} as the image; sd-cli writes every frame.`);
    if (form.lockSeed === false) notes.push("--seed -1 picks a random seed each run.");

    const exe = windows ? ".\\sd-cli.exe" : "./sd-cli";
    const continuation = windows ? " `" : " \\";
    const lines = [exe, ...args.map((arg) => arg.map((part, index) => (index === 0 ? part : quote(part, windows))).join(" "))];
    const cli = lines.join(`${continuation}\n    `);

    const kind = request?.kind || (video ? "vid_gen" : "img_gen");
    const body = JSON.parse(JSON.stringify(request?.body || {}));
    if (body.init_image) body.init_image = "data:image/...;base64,<your start image>";
    if (body.end_image) body.end_image = "data:image/...;base64,<your end frame>";
    if (Array.isArray(body.ref_audios)) body.ref_audios = ["data:audio/wav;base64,<your audio as WAV>"];
    if (Array.isArray(body.control_frames)) body.control_frames = [`<${body.control_frames.length} frames as image data URLs>`];
    if (Array.isArray(body.ref_images)) body.ref_images = body.ref_images.map((_: unknown, index: number) => `data:image/...;base64,<reference image ${index + 1}>`);
    const endpoint = `${window.location.origin}/sdcpp/v1/${kind}`;
    const requestJson = JSON.stringify(body, null, 2);
    const curl = windows
        ? `curl.exe -X POST ${endpoint} -H "Content-Type: application/json" --data "@request.json"`
        : `curl -X POST ${endpoint} -H 'Content-Type: application/json' --data @request.json`;
    if (!request) notes.unshift("The form is not complete yet (prompt or a required model file is missing).");
    return { windows, cli, notes, endpoint, requestJson, curl };
}
