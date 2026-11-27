import { computed, reactive, watch } from "vue";

import { api } from "./api";
import { availableTasks, producesAudio } from "./tasks";
import type { TaskId } from "./tasks";
import type { DownloadCatalog, Job, JobSummaryEntry, ModelFile, OutputEntry, Recipe } from "./types";

export type Aspect = "16:9" | "9:16" | "1:1" | "4:3" | "3:4";
export type SizePreset = "small" | "normal";

export interface CreateForm {
    recipeId: string;
    mode: "video" | "image";
    task: TaskId;
    strength: number;
    prompt: string;
    negativePrompt: string;
    aspect: Aspect;
    size: SizePreset;
    seconds: number;
    seed: number;
    lockSeed: boolean;
    steps: number | null;
    cfg: number | null;
    fps: number | null;
    submittedPrompt: string;
    // Per-model file choices that replace a preset's defaults; "" means "no file" for a slot.
    files: Record<string, Record<string, string>>;
}

const FORM_KEY = "sdcpp-studio-form";

function loadForm(): CreateForm {
    const fallback: CreateForm = {
        recipeId: "",
        mode: "video",
        task: "t2v",
        strength: 0.75,
        prompt: "",
        negativePrompt: "",
        aspect: "16:9",
        size: "normal",
        seconds: 2,
        seed: -1,
        lockSeed: false,
        steps: null,
        cfg: null,
        fps: null,
        submittedPrompt: "",
        files: {},
    };
    try {
        const saved = JSON.parse(localStorage.getItem(FORM_KEY) || "null");
        return saved && typeof saved === "object" ? { ...fallback, ...saved } : fallback;
    } catch {
        return fallback;
    }
}

export const state = reactive({
    recipes: [] as Recipe[],
    files: [] as ModelFile[],
    jobs: [] as JobSummaryEntry[],
    activeJob: null as Job | null,
    outputs: [] as OutputEntry[],
    outputsEnabled: true,
    downloads: null as DownloadCatalog | null,
    form: loadForm(),
    error: "",
    lastCompleted: null as OutputEntry | null,
    createSelected: null as OutputEntry | null,
    queueOpen: false,
    // True until the first load of each list finishes, so pages can show a loader instead of "empty".
    loading: { catalog: true, outputs: true, downloads: true },
    // Optional input images as data URLs; kept in memory only (too large for local storage).
    inputs: {
        startImage: "" as string,
        startName: "" as string,
        endImage: "" as string,
        endName: "" as string,
        refImages: [] as { name: string; data: string }[],
        audio: null as { name: string; data: string; seconds: number } | null,
        control: null as { name: string; frames: string[] } | null,
    },
});

watch(
    () => state.form,
    (form) => {
        try {
            localStorage.setItem(FORM_KEY, JSON.stringify(form));
        } catch {
            /* storage may be unavailable */
        }
    },
    { deep: true },
);

// ---------- recipes ----------

const fileIds = computed(() => new Set(state.files.map((f) => f.id)));

export function recipeReady(recipe: Recipe): boolean {
    const models = recipe.models || {};
    const has = (slot: string) => Boolean(models[slot]) && fileIds.value.has(models[slot]);
    if (!recipe.slots.filter((s) => s.required).every((s) => has(s.id))) return false;
    return (recipe.required_any || []).every((group) => group.some(has));
}

// Distilled video models can produce noisy single frames; these render a short clip and keep one frame.
const STILL_CLIPS: { pattern: RegExp; frames: number; keep: number }[] = [{ pattern: /kandinsky6/, frames: 5, keep: 2 }];

export function stillClip(recipe: Recipe | undefined) {
    return STILL_CLIPS.find((s) => recipe && s.pattern.test(recipe.id)) || null;
}

export function recipeIsVideo(recipe: Recipe | undefined): boolean {
    return Boolean(recipe && (recipe.video_constraints || recipe.defaults?.video_frames));
}

export const readyRecipes = computed(() =>
    state.recipes.filter(recipeReady).sort((a, b) => a.name.localeCompare(b.name)),
);

export const currentRecipe = computed(
    () => state.recipes.find((r) => r.id === state.form.recipeId) || readyRecipes.value[0],
);

// The files the next job will use: the preset's defaults overlaid with the user's choices.
export const selectedFiles = computed<Record<string, string>>(() => {
    const recipe = currentRecipe.value;
    if (!recipe) return {};
    const chosen = state.form.files[recipe.id] || {};
    const result: Record<string, string> = {};
    for (const slot of recipe.slots) {
        const value = slot.id in chosen ? chosen[slot.id] : recipe.models?.[slot.id] || "";
        result[slot.id] = value && fileIds.value.has(value) ? value : "";
    }
    return result;
});

// Human-readable reasons the current selection cannot run yet.
export const missingFiles = computed<string[]>(() => {
    const recipe = currentRecipe.value;
    if (!recipe) return [];
    const files = selectedFiles.value;
    const missing = recipe.slots.filter((s) => s.required && !files[s.id]).map((s) => s.label || s.id);
    for (const group of recipe.required_any || [])
        if (!group.some((id) => files[id]))
            missing.push(group.map((id) => recipe.slots.find((s) => s.id === id)?.label || id).join(" or "));
    return missing;
});

export function chooseFile(slot: string, file: string) {
    const recipe = currentRecipe.value;
    if (!recipe) return;
    state.form.files = { ...state.form.files, [recipe.id]: { ...(state.form.files[recipe.id] || {}), [slot]: file } };
}

export function resetFileChoices() {
    const recipe = currentRecipe.value;
    if (!recipe) return;
    const next = { ...state.form.files };
    delete next[recipe.id];
    state.form.files = next;
}

export const hasFileChanges = computed(() => {
    const recipe = currentRecipe.value;
    return Boolean(recipe && Object.keys(state.form.files[recipe.id] || {}).length);
});

// ---------- geometry ----------

const ASPECTS: Record<Aspect, number> = { "16:9": 16 / 9, "9:16": 9 / 16, "1:1": 1, "4:3": 4 / 3, "3:4": 3 / 4 };

export function frameSize(recipe: Recipe | undefined, aspect: Aspect, size: SizePreset) {
    const multiple = recipe?.size_multiple || 16;
    const baseW = recipe?.defaults?.width || 832;
    const baseH = recipe?.defaults?.height || 480;
    const area = baseW * baseH * (size === "small" ? 0.5 : 1);
    const ratio = ASPECTS[aspect];
    const snap = (v: number) => Math.max(multiple, Math.round(v / multiple) * multiple);
    return { width: snap(Math.sqrt(area * ratio)), height: snap(Math.sqrt(area / ratio)) };
}

export function alignFrames(frames: number, recipe: Recipe | undefined): number {
    const c = recipe?.video_constraints || {};
    const step = c.frame_step ?? 1;
    const offset = c.frame_offset ?? 0;
    const first = offset + Math.max(0, Math.ceil(((c.min_frames ?? 1) - offset) / step)) * step;
    const last = offset + Math.floor(((c.max_frames ?? 4096) - offset) / step) * step;
    const units = (frames - offset) / step;
    const rounded = c.frame_rounding === "ceil" ? Math.ceil(units - 1e-9) : Math.floor(units + 1e-9);
    return Math.min(last, Math.max(first, rounded * step + offset));
}

export const plan = computed(() => {
    const recipe = currentRecipe.value;
    const form = state.form;
    const { width, height } = frameSize(recipe, form.aspect, form.size);
    const fps = recipe?.video_constraints?.fixed_fps ?? form.fps ?? recipe?.defaults?.fps ?? 24;
    const video = currentTask.value?.output === "video" && recipeIsVideo(recipe);
    const still = video ? null : stillClip(recipe);
    const frames = video ? alignFrames(Math.max(1, Math.round(form.seconds * fps)), recipe) : 1;
    const renderFrames = still ? alignFrames(still.frames, recipe) : frames;
    const steps = form.steps ?? recipe?.defaults?.steps ?? 20;
    return { recipe, width, height, fps, frames, renderFrames, still, steps, video, seconds: frames / fps };
});

// What the chosen config accepts, from the server's resolved recipe features.
// Tasks the chosen model offers, and the one in use (falls back to the first available).
export const tasks = computed(() => availableTasks(currentRecipe.value));
export const currentTask = computed(() => tasks.value.find((t) => t.id === state.form.task) || tasks.value[0]);

// What the chosen model accepts for the current task.
export const inputSupport = computed(() => {
    const features = currentRecipe.value?.effective_features || {};
    const task = currentTask.value?.id;
    // VACE models take one reference picture through the start-image input.
    const vaceReference = task === "r2v" && Boolean(features.vace) && !features.ref_images;
    return {
        startFrame: task === "i2v" || task === "s2v" || task === "i2i" || vaceReference || (task === "v2v" && Boolean(features.init_image)),
        startRequired: task === "i2v" || task === "s2v" || task === "i2i" || vaceReference,
        startLabel:
            task === "i2i" ? "Image to redraw" : task === "s2v" ? "Portrait (who speaks)" : vaceReference ? "Character or object to keep" : task === "v2v" ? "Reference image" : "Start frame",
        vaceReference,
        endFrame: task === "i2v" && Boolean(features.end_image),
        refImages: (task === "edit" || task === "r2v") && Boolean(features.ref_images),
        audio: task === "s2v" || (task === "r2v" && Boolean(features.ref_audios)),
        control: task === "v2v",
        strength: task === "i2i",
        sound: producesAudio(currentRecipe.value, selectedFiles.value),
    };
});

// Inputs the current task still needs before it can run.
export const missingInputs = computed<string[]>(() => {
    const support = inputSupport.value;
    const inputs = state.inputs;
    const missing: string[] = [];
    if (support.startRequired && !inputs.startImage) missing.push(support.startLabel.toLowerCase());
    if (currentTask.value?.id === "edit" && !inputs.refImages.length) missing.push("at least one image to edit");
    if (currentTask.value?.id === "r2v" && !support.vaceReference && !inputs.refImages.length && !inputs.audio) missing.push("a reference image or audio clip");
    if (currentTask.value?.id === "s2v" && !inputs.audio) missing.push("a speech audio clip");
    if (support.control && !inputs.control) missing.push("a control video");
    return missing;
});

export function buildRequest(): { kind: "img_gen" | "vid_gen"; body: Record<string, unknown> } | null {
    const p = plan.value;
    const form = state.form;
    if (!p.recipe || !form.prompt.trim() || missingFiles.value.length || missingInputs.value.length) return null;
    const usesVideoEndpoint = recipeIsVideo(p.recipe);
    const body: Record<string, unknown> = {
        prompt: form.prompt.trim(),
        negative_prompt: form.negativePrompt,
        width: p.width,
        height: p.height,
        seed: form.lockSeed ? form.seed : -1,
        model: { recipe: p.recipe.id, models: { ...selectedFiles.value } },
        preview: "proj",
        preview_interval: 1,
    };
    const sample: Record<string, unknown> = {};
    if (form.steps != null) sample.sample_steps = form.steps;
    if (form.cfg != null) sample.guidance = { txt_cfg: form.cfg };
    if (Object.keys(sample).length) body.sample_params = sample;
    const support = inputSupport.value;
    const inputs = state.inputs;
    if (support.startFrame && inputs.startImage) body.init_image = inputs.startImage;
    if (support.endFrame && inputs.endImage) body.end_image = inputs.endImage;
    if (support.refImages && inputs.refImages.length) body.ref_images = inputs.refImages.map((image) => image.data);
    if (support.audio && inputs.audio) body.ref_audios = [inputs.audio.data];
    if (support.control && inputs.control) body.control_frames = inputs.control.frames;
    if (support.strength) body.strength = form.strength;
    if (usesVideoEndpoint) {
        body.video_frames = p.renderFrames;
        body.fps = p.fps;
        body.output_format = p.renderFrames === 1 ? "webp" : "webm";
        if (p.still) body.still_frame = p.still.keep;
        return { kind: "vid_gen", body };
    }
    body.output_format = "png";
    return { kind: "img_gen", body };
}

// ---------- time estimates ----------

type JobEntryLike = { recipe?: string; width: number | null; height: number | null; steps: number | null; video_frames?: number | null };

const median = (values: number[]) => {
    if (!values.length) return null;
    const sorted = [...values].sort((a, b) => a - b);
    return sorted[Math.floor(sorted.length / 2)];
};

// Per-recipe timing model learned from saved results:
//   seconds = fixed + step * steps * workload + decode * workload, with workload = pixels x frames.
function timingModel(recipe: string | undefined) {
    const samples = state.outputs
        .filter((o) => o.recipe === recipe && o.stage_ms && o.width && o.height && o.steps)
        .slice(0, 12);
    if (!samples.length) return null;
    const workload = (o: OutputEntry) => o.width! * o.height! * (o.rendered_frames || o.frame_count || o.video_frames || 1);
    const ms = (o: OutputEntry, key: string) => (o.stage_ms?.[key] || 0) / 1000;
    return {
        fixed: median(samples.map((o) => ms(o, "waiting") + ms(o, "loading") + ms(o, "preparing") + ms(o, "conditioning")))!,
        step: median(samples.map((o) => ms(o, "sampling") / (o.steps! * workload(o))))!,
        decode: median(samples.map((o) => (ms(o, "vae_decode") + ms(o, "encoding")) / workload(o)))!,
    };
}

function workloadOf(s: JobEntryLike) {
    return s.width && s.height ? s.width * s.height * Math.max(1, s.video_frames || 1) : null;
}

export function estimateTotalSeconds(summary: JobEntryLike): number | null {
    const model = timingModel(summary.recipe);
    const work = workloadOf(summary);
    if (!model || work == null || !summary.steps) return null;
    return model.fixed + model.step * summary.steps * work + model.decode * work;
}

export function remainingSeconds(job: JobSummaryEntry): number | null {
    const s = job.summary;
    if (!s) return null;
    const total = estimateTotalSeconds(s);
    if (job.status === "queued") return total;
    if (job.status !== "generating" || !job.progress) return null;
    const p = job.progress;
    const model = timingModel(s.recipe);
    const work = workloadOf(s);
    const decode = model && work != null ? model.decode * work : null;
    if (p.stage === "sampling" && p.total) {
        const perStep = p.completed ? p.stage_elapsed_ms / 1000 / p.completed : model && work != null ? model.step * work : null;
        if (perStep == null) return null;
        return Math.max(0, (p.total - (p.completed || 0)) * perStep + (decode ?? perStep * 2));
    }
    if (p.stage === "vae_decode" || p.stage === "encoding") return decode != null ? Math.max(0, decode - p.stage_elapsed_ms / 1000) : null;
    return total != null ? Math.max(0, total - p.elapsed_ms / 1000) : null;
}

export function queueWaitSeconds(job: JobSummaryEntry): number | null {
    let sum = 0;
    for (const other of state.jobs) {
        if (other.id === job.id) break;
        if (other.status !== "queued" && other.status !== "generating") continue;
        const r = remainingSeconds(other);
        if (r == null) return null;
        sum += r;
    }
    return sum;
}

export function formatDuration(seconds: number | null | undefined): string {
    if (seconds == null || !Number.isFinite(seconds)) return "—";
    const s = Math.round(seconds);
    if (s < 60) return `${s}s`;
    const m = Math.floor(s / 60);
    if (m < 60) return `${m}m ${String(s % 60).padStart(2, "0")}s`;
    return `${Math.floor(m / 60)}h ${String(m % 60).padStart(2, "0")}m`;
}

export function eta(seconds: number | null | undefined): string {
    return seconds == null || !Number.isFinite(seconds) ? "estimating…" : `≈ ${formatDuration(seconds)}`;
}

export function formatBytes(bytes: number): string {
    if (!bytes) return "0 B";
    const units = ["B", "KB", "MB", "GB", "TB"];
    const i = Math.min(units.length - 1, Math.floor(Math.log(bytes) / Math.log(1024)));
    return `${(bytes / 1024 ** i).toFixed(i >= 3 ? 1 : 0)} ${units[i]}`;
}

export const stageLabels: Record<string, string> = {
    queued: "Waiting in queue",
    waiting: "Waiting",
    preparing: "Loading models",
    conditioning: "Reading the prompt",
    sampling: "Generating",
    vae_decode: "Decoding frames",
    vae_encode: "Encoding input",
    audio_decode: "Decoding audio",
    encoding: "Saving",
    upscaling: "Upscaling",
};

// ---------- loading and polling ----------

export async function loadCatalog(refresh = false) {
    try {
        if (refresh) await api.refreshModels();
        const [recipes, files] = await Promise.all([api.recipes(), api.files()]);
        state.recipes = recipes.recipes;
        state.files = files.files;
        if (!state.recipes.some((r) => r.id === state.form.recipeId) && readyRecipes.value.length) {
            const preferred = readyRecipes.value.find((r) => /kandinsky6.*fast/.test(r.id)) || readyRecipes.value[0];
            state.form.recipeId = preferred.id;
        }
    } catch (error) {
        state.error = String((error as Error).message || error);
    } finally {
        state.loading.catalog = false;
    }
}

export async function loadOutputs() {
    try {
        const result = await api.outputs();
        state.outputs = result.outputs;
        state.outputsEnabled = result.enabled;
    } catch {
        /* keep the previous list */
    } finally {
        state.loading.outputs = false;
    }
}

export async function loadDownloads() {
    try {
        state.downloads = await api.downloads();
    } catch (error) {
        state.error = String((error as Error).message || error);
    } finally {
        state.loading.downloads = false;
    }
}

export async function submit(): Promise<string | null> {
    const request = buildRequest();
    if (!request) return null;
    try {
        const job = await api.submit(request.kind, request.body);
        state.form.submittedPrompt = state.form.prompt;
        state.error = "";
        await pollJobs();
        return job.id;
    } catch (error) {
        state.error = String((error as Error).message || error);
        return null;
    }
}

export async function cancel(id: string) {
    try {
        await api.cancelJob(id);
    } finally {
        await pollJobs();
    }
}

const seenStatus = new Map<string, string>();

export async function pollJobs() {
    try {
        const { jobs } = await api.jobs();
        state.jobs = jobs;
        let finished = false;
        for (const job of jobs) {
            const before = seenStatus.get(job.id);
            if (before && before !== job.status && job.status === "completed") finished = true;
            seenStatus.set(job.id, job.status);
        }
        const running = jobs.find((j) => j.status === "generating");
        state.activeJob = running ? await api.job(running.id) : null;
        if (finished) {
            await loadOutputs();
            state.lastCompleted = state.outputs[0] || null;
        }
    } catch {
        /* server restarting; try again on the next tick */
    }
}

export const activeJobs = computed(() => state.jobs.filter((j) => j.status === "queued" || j.status === "generating"));

let timer: number | undefined;
export function startPolling() {
    const tick = async () => {
        await pollJobs();
        const busy = activeJobs.value.length > 0;
        const downloading = state.downloads?.packages.some((p) => ["queued", "starting", "downloading", "verifying", "packing"].includes(p.status.state));
        if (downloading) {
            const before = new Set(state.downloads!.packages.filter((p) => p.installed).map((p) => p.id));
            await loadDownloads();
            if (state.downloads!.packages.some((p) => p.installed && !before.has(p.id))) await loadCatalog();
        }
        timer = window.setTimeout(tick, busy || downloading ? 1000 : 4000);
    };
    window.clearTimeout(timer);
    tick();
}

// Starts a fresh Create page: clears the prompt, per-run overrides and the shown result,
// keeping the chosen model, shape, size and length.
export function newCreation() {
    const form = state.form;
    state.inputs.startImage = "";
    state.inputs.startName = "";
    state.inputs.endImage = "";
    state.inputs.endName = "";
    state.inputs.refImages = [];
    state.inputs.audio = null;
    state.inputs.control = null;
    form.prompt = "";
    form.negativePrompt = "";
    form.submittedPrompt = "";
    form.lockSeed = false;
    form.steps = null;
    form.cfg = null;
    form.fps = null;
    state.lastCompleted = null;
    state.createSelected = null;
    state.error = "";
}

// Entering Create clears it only when its prompt was already generated, so unsent drafts survive.
export function enterCreate() {
    const form = state.form;
    if (!form.prompt.trim() || form.prompt === form.submittedPrompt) newCreation();
    else {
        state.lastCompleted = null;
        state.createSelected = null;
    }
}

export async function deleteOutput(output: OutputEntry) {
    await api.deleteOutput(output.file);
    if (state.lastCompleted?.file === output.file) state.lastCompleted = null;
    if (state.createSelected?.file === output.file) state.createSelected = null;
    await loadOutputs();
}

export function reuse(output: OutputEntry) {
    const form = state.form;
    form.prompt = output.prompt || "";
    form.submittedPrompt = "";
    form.negativePrompt = output.negative_prompt || "";
    if (output.recipe && state.recipes.some((r) => r.id === output.recipe)) {
        form.recipeId = output.recipe;
        if (output.models && Object.keys(output.models).length) form.files = { ...form.files, [output.recipe]: { ...output.models } };
    }
    const frames = output.frame_count || output.video_frames || 1;
    form.mode = frames > 1 ? "video" : "image";
    form.task = frames > 1 ? "t2v" : "t2i";
    if (frames > 1 && output.fps) form.seconds = Math.round((frames / output.fps) * 10) / 10;
    if (output.width && output.height) {
        const r = output.width / output.height;
        form.aspect = r > 1.5 ? "16:9" : r > 1.1 ? "4:3" : r > 0.9 ? "1:1" : r > 0.65 ? "3:4" : "9:16";
    }
    restoreInputs(output, frames > 1);
}

async function urlToDataUrl(url: string): Promise<{ data: string; bytes: ArrayBuffer }> {
    const response = await fetch(url);
    if (!response.ok) throw new Error(`HTTP ${response.status}`);
    const blob = await response.blob();
    const bytes = await blob.arrayBuffer();
    const data = await new Promise<string>((resolve, reject) => {
        const reader = new FileReader();
        reader.onload = () => resolve(String(reader.result));
        reader.onerror = () => reject(reader.error);
        reader.readAsDataURL(blob);
    });
    return { data, bytes };
}

// Puts the inputs a result was made from (kept in the inputs folder) back into the form.
async function restoreInputs(output: OutputEntry, video: boolean) {
    const saved = output.inputs || {};
    const inputs = state.inputs;
    const name = (url: string) => url.split("/").pop() || "input";
    inputs.startImage = inputs.startName = inputs.endImage = inputs.endName = "";
    inputs.refImages = [];
    inputs.audio = null;
    inputs.control = null;
    const form = state.form;
    if (saved.control_frames?.length) form.task = "v2v";
    else if (saved.ref_audios?.length && saved.init_image) form.task = "s2v";
    else if (saved.ref_images?.length || saved.ref_audios?.length) form.task = video ? "r2v" : "edit";
    else if (saved.init_image) form.task = video ? "i2v" : "i2i";
    try {
        if (saved.init_image) [inputs.startImage, inputs.startName] = [(await urlToDataUrl(saved.init_image)).data, name(saved.init_image)];
        if (saved.end_image) [inputs.endImage, inputs.endName] = [(await urlToDataUrl(saved.end_image)).data, name(saved.end_image)];
        for (const url of saved.ref_images || []) inputs.refImages.push({ name: name(url), data: (await urlToDataUrl(url)).data });
        if (saved.ref_audios?.length) {
            const { data, bytes } = await urlToDataUrl(saved.ref_audios[0]);
            const view = new DataView(bytes);
            const byteRate = bytes.byteLength > 44 ? view.getUint32(28, true) : 0;
            inputs.audio = { name: name(saved.ref_audios[0]), data, seconds: byteRate ? (bytes.byteLength - 44) / byteRate : 0 };
        }
        if (saved.control_frames?.length) {
            const frames: string[] = [];
            for (const url of saved.control_frames) frames.push((await urlToDataUrl(url)).data);
            inputs.control = { name: `${frames.length} saved frames`, frames };
        }
    } catch (error) {
        state.error = `Some inputs of this result could not be loaded: ${(error as Error).message}`;
    }
}
