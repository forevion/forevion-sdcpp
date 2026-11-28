<script setup lang="ts">
import { computed, onMounted, reactive, ref, watch } from "vue";

import Icon from "./Icon.vue";
import { api } from "../lib/api";
import { categoryLabel } from "../lib/categories";
import { loadCatalog, state } from "../lib/store";
import type { DownloadPackage, ModelSlotDef, Recipe } from "../lib/types";

// "new": a blank model config listing every input type, optionally filled from a download's guide.
// "edit": change an existing config. "copy": a new config starting from `base`.
const props = defineProps<{ mode: "new" | "edit" | "copy"; base?: Recipe; guidePackage?: string }>();
const emit = defineEmits<{ (e: "close"): void; (e: "saved", id: string): void }>();

const RESPONSE_ONLY = ["source", "file", "root", "effective_features"];
// Input groups for blank configs, in the order people usually fill them in.
const GROUPS: { title: string; slots: string[]; more?: boolean }[] = [
    { title: "Main model", slots: ["diffusion_model", "model"] },
    { title: "Text encoders", slots: ["t5xxl", "llm", "clip_l", "clip_g", "tokenizer"] },
    { title: "Decoder", slots: ["vae", "tae"] },
    { title: "Vision and audio", slots: ["clip_vision", "llm_vision", "audio_vae", "audio_encoder", "embeddings_connectors"], more: true },
    { title: "Extras", slots: ["high_noise_diffusion_model", "uncond_diffusion_model", "control_net", "ip_adapter", "motion_module", "photo_maker", "pulid_weights"], more: true },
];
const INPUT_HINTS: Record<string, string> = {
    diffusion_model: "The transformer / UNet weights (e.g. FastWan, Wan 2.2, Kandinsky).",
    model: "Only for all-in-one checkpoints that already contain the text encoder and VAE.",
    t5xxl: "T5 / UMT5 text encoder (Wan, Flux, SD3).",
    llm: "LLM text encoder such as Qwen (Kandinsky 6, Qwen-Image).",
    clip_l: "CLIP-L text encoder (Kandinsky 6, Flux, SD3).",
    vae: "Full VAE decoder: sharpest, slower.",
    tae: "Tiny decoder: decodes in seconds, a little softer.",
    tokenizer: "Only when the model guide asks for a separate tokenizer.json.",
};

function slugify(text: string) {
    return text.toLowerCase().replace(/[^a-z0-9]+/g, "-").replace(/^-+|-+$/g, "").slice(0, 100) || "config";
}

const slotDefs = ref<ModelSlotDef[]>([]);
const samplers = ref<string[]>([]);
const schedulers = ref<string[]>([]);
const error = ref("");
const saving = ref(false);
const showMore = ref(false);
const guideId = ref(props.guidePackage || "");

// Fields of the base config the editor does not show (constraints, features, context...) are kept.
const extras = ref<Record<string, any>>({});
const baseSlots = ref<{ id: string; label?: string; required: boolean; categories?: string[] }[]>([]);

const form = reactive({
    name: "",
    id: "",
    idTouched: false,
    description: "",
    video: true,
    models: {} as Record<string, string>,
    width: null as number | null,
    height: null as number | null,
    steps: null as number | null,
    cfg: null as number | null,
    sampler: "",
    scheduler: "",
    frames: null as number | null,
    fps: null as number | null,
    flowShift: null as number | null,
    abilities: [] as string[],
});

// Abilities map to recipe features; they decide which tasks Create offers for this variant.
const ABILITIES = [
    { id: "init_image", label: "Start from an image (image → video)", video: true },
    { id: "end_image", label: "End on an image (first/last frame)", video: true },
    { id: "ref_audios", label: "Speech / audio input (lip sync)", video: true },
    { id: "control_frames", label: "Follow a control video (video → video)", video: true },
    { id: "ref_images", label: "Reference or edit images", video: null },
    { id: "img2img", label: "Redraw an image (image → image)", video: false },
] as const;

function loadAbilities() {
    const declared = new Set<string>(extras.value.features || []);
    const disabled = new Set<string>(extras.value.disabled_features || []);
    form.abilities = ABILITIES.filter((a) => (a.id === "img2img" ? !disabled.has("img2img") : declared.has(a.id))).map((a) => a.id);
}

const guides = computed(() => (state.downloads?.packages || []).filter((p) => p.guide?.inputs));
const guide = computed<DownloadPackage | undefined>(() => guides.value.find((p) => p.id === guideId.value));
const fromGuide = (slot: string) => Boolean(guide.value?.guide?.inputs?.[slot]) && guide.value!.guide!.inputs![slot] === form.models[slot];

const slotList = computed(() => {
    if (props.mode !== "new") return baseSlots.value;
    return slotDefs.value.filter((s) => s.id !== "taesd").map((s) => ({ id: s.id, label: s.label, required: false, categories: s.categories }));
});
const groups = computed(() => {
    if (props.mode !== "new") return [{ title: "Model files", slots: slotList.value, more: false }];
    return GROUPS.map((g) => ({ ...g, slots: g.slots.map((id) => slotList.value.find((s) => s.id === id)).filter(Boolean) as typeof slotList.value }));
});
const idTaken = computed(() => props.mode !== "edit" && state.recipes.some((r) => r.id === form.id && r.source === "user"));
const overridesBuiltin = computed(() => props.mode !== "edit" && state.recipes.some((r) => r.id === form.id && r.source === "builtin"));

function filesFor(categories: string[] | undefined) {
    const allowed = new Set(categories || []);
    return state.files.filter((f) => allowed.has(f.category)).sort((a, b) => (a.name || a.id).localeCompare(b.name || b.id));
}

function fileExists(id: string) {
    return state.files.some((f) => f.id === id);
}

function loadDefaults(defaults: Recipe["defaults"] = {}) {
    form.width = defaults.width ?? null;
    form.height = defaults.height ?? null;
    form.steps = defaults.steps ?? null;
    form.cfg = defaults.cfg_scale ?? null;
    form.sampler = defaults.sampling_method ?? "";
    form.scheduler = defaults.scheduler ?? "";
    form.frames = defaults.video_frames ?? null;
    form.fps = defaults.fps ?? null;
    form.flowShift = defaults.flow_shift ?? null;
}

function applyGuide() {
    const pkg = guide.value;
    if (!pkg?.guide) return;
    const settings = pkg.guide.settings || {};
    for (const key of Object.keys(form.models)) form.models[key] = "";
    for (const [slot, file] of Object.entries(pkg.guide.inputs || {})) form.models[slot] = file;
    loadDefaults(settings.defaults);
    const { defaults: _ignored, ...rest } = settings;
    extras.value = { ...rest, defaults: { ...(settings.defaults || {}) } };
    loadAbilities();
    form.video = Boolean(settings.video_constraints || settings.defaults?.video_frames);
    if (!form.name || props.mode === "new") {
        form.name = pkg.name;
        if (!form.idTouched) form.id = slugify(pkg.name);
    }
}

function useAlternative(slot: string, file: string) {
    form.models[slot] = file;
    if (slot === "vae" && form.models.tae) form.models.tae = "";
    if (slot === "tae" && form.models.vae) form.models.vae = "";
}

function onName() {
    if (props.mode !== "edit" && !form.idTouched) form.id = slugify(form.name);
}

function setNumber(target: Record<string, unknown>, key: string, value: number | null) {
    if (value === null || (value as unknown) === "" || Number.isNaN(Number(value))) delete target[key];
    else target[key] = Number(value);
}

function setText(target: Record<string, unknown>, key: string, value: string) {
    if (value) target[key] = value;
    else delete target[key];
}

async function save() {
    error.value = "";
    if (!form.name.trim()) return void (error.value = "Give the variant a name.");
    if (!/^[A-Za-z0-9_.-]{1,128}$/.test(form.id)) return void (error.value = "The ID may use letters, digits, dots, dashes and underscores.");
    if (idTaken.value) return void (error.value = "You already have a variant with this ID.");

    const recipe: Record<string, any> = { ...extras.value, schema_version: 1, id: form.id, name: form.name.trim() };
    const features = new Set<string>(recipe.features || []);
    const disabled = new Set<string>(recipe.disabled_features || []);
    for (const ability of ABILITIES) {
        const on = form.abilities.includes(ability.id) && (ability.video === null || ability.video === form.video);
        if (ability.id === "img2img") {
            if (on) disabled.delete("img2img");
            else disabled.add("img2img");
        } else if (on) features.add(ability.id);
        else features.delete(ability.id);
    }
    if (features.has("control_frames")) features.add("vace");
    if (features.size) recipe.features = [...features];
    else delete recipe.features;
    if (disabled.size) recipe.disabled_features = [...disabled];
    else delete recipe.disabled_features;
    setText(recipe, "description", form.description.trim());
    if (props.mode === "new") {
        const chosen = slotList.value.filter((s) => form.models[s.id]);
        if (!chosen.some((s) => s.id === "diffusion_model" || s.id === "model"))
            return void (error.value = "Choose a Diffusion model (or an all-in-one Checkpoint).");
        recipe.slots = chosen.map((s) => ({ id: s.id, required: true }));
        delete recipe.required_any;
    }
    const models: Record<string, string> = {};
    for (const slot of recipe.slots || []) if (form.models[slot.id]) models[slot.id] = form.models[slot.id];
    if (Object.keys(models).length) recipe.models = models;
    else delete recipe.models;

    const defaults: Record<string, unknown> = { ...(recipe.defaults || {}) };
    setNumber(defaults, "width", form.width);
    setNumber(defaults, "height", form.height);
    setNumber(defaults, "steps", form.steps);
    setNumber(defaults, "cfg_scale", form.cfg);
    setNumber(defaults, "flow_shift", form.flowShift);
    setText(defaults, "sampling_method", form.sampler);
    setText(defaults, "scheduler", form.scheduler);
    if (form.video) {
        setNumber(defaults, "video_frames", form.frames);
        setNumber(defaults, "fps", form.fps);
    } else {
        delete defaults.video_frames;
        delete defaults.fps;
        delete recipe.video_constraints;
    }
    if (Object.keys(defaults).length) recipe.defaults = defaults;
    else delete recipe.defaults;

    saving.value = true;
    try {
        await api.saveRecipe(recipe);
        await loadCatalog();
        emit("saved", form.id);
        emit("close");
    } catch (e) {
        error.value = (e as Error).message;
    } finally {
        saving.value = false;
    }
}

watch(guideId, () => applyGuide());

onMounted(async () => {
    const [slots, caps] = await Promise.allSettled([api.modelSlots(), api.capabilities()]);
    if (slots.status === "fulfilled") slotDefs.value = slots.value.slots;
    if (caps.status === "fulfilled") {
        samplers.value = caps.value.samplers || [];
        schedulers.value = caps.value.schedulers || [];
    }
    if (props.mode === "new") {
        for (const s of slotDefs.value) form.models[s.id] = "";
        extras.value = {};
        if (guideId.value) applyGuide();
        return;
    }
    const base = JSON.parse(JSON.stringify(props.base)) as Record<string, any>;
    for (const key of RESPONSE_ONLY) delete base[key];
    baseSlots.value = (props.base?.slots || []).map((s) => ({ id: s.id, label: s.label, required: s.required, categories: s.categories }));
    for (const slot of base.slots || []) {
        delete slot.categories;
        delete slot.category;
    }
    const { id: _id, name: _name, description: _description, ...rest } = base;
    extras.value = rest;
    loadAbilities();
    form.name = props.mode === "copy" ? `${props.base!.name} (my copy)` : props.base!.name;
    form.id = props.mode === "copy" ? slugify(`${props.base!.id}-custom`) : props.base!.id;
    form.description = props.base!.description || "";
    form.video = Boolean(props.base!.video_constraints || props.base!.defaults?.video_frames);
    for (const s of baseSlots.value) form.models[s.id] = props.base!.models?.[s.id] || "";
    loadDefaults(props.base!.defaults);
});
</script>

<template>
    <div class="overlay" role="dialog" aria-modal="true" aria-labelledby="preset-title" @click.self="emit('close')">
        <form class="dialog panel" @submit.prevent="save">
            <header class="head">
                <div>
                    <h2 id="preset-title">{{ mode === "edit" ? "Edit variant" : "New variant" }}</h2>
                    <p class="faint small">
                        <template v-if="mode === 'copy'">Starting from {{ base?.name }}. </template>
                        Pick a file for each input the model needs, then save.
                    </p>
                </div>
                <button class="btn-ghost icon-btn" type="button" aria-label="Close" @click="emit('close')"><Icon name="close" /></button>
            </header>

            <div class="body">
                <section v-if="mode === 'new'" class="guide panel">
                    <label class="field">
                        <span>Help guide (optional)</span>
                        <select v-model="guideId" class="input">
                            <option value="">Start empty</option>
                            <option v-for="p in guides" :key="p.id" :value="p.id">{{ p.name }}{{ p.installed ? "" : " (not downloaded yet)" }}</option>
                        </select>
                    </label>
                    <template v-if="guide?.guide">
                        <ul class="notes">
                            <li v-for="note in guide.guide.notes" :key="note">{{ note }}</li>
                        </ul>
                        <div v-if="guide.guide.alternatives" class="alts">
                            <span class="faint small">Alternatives:</span>
                            <button
                                v-for="(file, slot) in guide.guide.alternatives"
                                :key="slot"
                                class="btn-ghost alt"
                                type="button"
                                :disabled="!fileExists(file)"
                                @click="useAlternative(String(slot), file)"
                            >
                                Use {{ file.split("/").pop() }} as {{ slotList.find((s) => s.id === slot)?.label || slot }}
                            </button>
                        </div>
                        <p v-if="!guide.installed" class="warn small">Download this package on the Download tab first; its files are not on disk yet.</p>
                    </template>
                </section>

                <div class="row2">
                    <label class="field"><span>Name</span><input v-model="form.name" class="input" placeholder="e.g. FastWan quick clips" @input="onName" /></label>
                    <label class="field"><span>ID</span><input v-model="form.id" class="input mono" :disabled="mode === 'edit'" @input="form.idTouched = true" /></label>
                </div>
                <p v-if="overridesBuiltin" class="warn small">This ID matches a built-in model type; your variant will replace it until you delete yours.</p>
                <label class="field"><span>Description</span><textarea v-model="form.description" class="input" rows="2" /></label>

                <template v-for="group in groups" :key="group.title">
                    <template v-if="!group.more || showMore">
                        <h3>{{ group.title }}</h3>
                        <div class="slots">
                            <label v-for="slot in group.slots" :key="slot.id" class="field">
                                <span>
                                    {{ slot.label || slot.id }}
                                    <em v-if="mode !== 'new'" :class="slot.required ? 'req' : 'faint'">{{ slot.required ? "required" : "optional" }}</em>
                                    <em v-if="fromGuide(slot.id)" class="tag">from guide</em>
                                </span>
                                <select v-model="form.models[slot.id]" class="input" :class="{ missing: form.models[slot.id] && !fileExists(form.models[slot.id]) }">
                                    <option value="">{{ mode !== "new" && slot.required ? "— choose a file —" : "— not used —" }}</option>
                                    <option v-if="form.models[slot.id] && !fileExists(form.models[slot.id])" :value="form.models[slot.id]">
                                        {{ form.models[slot.id].split("/").pop() }} (not downloaded)
                                    </option>
                                    <option v-for="f in filesFor(slot.categories)" :key="f.id" :value="f.id">{{ f.name || f.id }}</option>
                                </select>
                                <small class="faint">
                                    {{ INPUT_HINTS[slot.id] || "" }}
                                    <template v-if="!filesFor(slot.categories).length"> No {{ (slot.categories || []).map(categoryLabel).join(" / ") }} files yet.</template>
                                </small>
                            </label>
                        </div>
                    </template>
                </template>
                <button v-if="mode === 'new'" class="btn-ghost more" type="button" @click="showMore = !showMore">
                    {{ showMore ? "Fewer inputs" : "More inputs (vision, audio, ControlNet and other extras)" }}
                </button>

                <h3>Abilities</h3>
                <p class="faint small">What this model can do; Create offers the matching tasks. Leave these as the guide or model type set them unless you know the model supports more.</p>
                <div class="abilities">
                    <label v-for="a in ABILITIES.filter((x) => x.video === null || x.video === form.video)" :key="a.id" class="check">
                        <input v-model="form.abilities" type="checkbox" :value="a.id" /> {{ a.label }}
                    </label>
                </div>

                <h3>Defaults</h3>
                <label class="check"><input v-model="form.video" type="checkbox" /> Makes video (set frames and FPS)</label>
                <div class="grid">
                    <label class="field"><span>Width</span><input v-model.number="form.width" class="input" type="number" min="64" max="4096" /></label>
                    <label class="field"><span>Height</span><input v-model.number="form.height" class="input" type="number" min="64" max="4096" /></label>
                    <label class="field"><span>Steps</span><input v-model.number="form.steps" class="input" type="number" min="1" max="1000" /></label>
                    <label class="field"><span>CFG</span><input v-model.number="form.cfg" class="input" type="number" min="0" max="1000" step="0.5" /></label>
                    <label class="field">
                        <span>Sampler</span>
                        <select v-model="form.sampler" class="input">
                            <option value="">model default</option>
                            <option v-for="s in samplers" :key="s" :value="s">{{ s }}</option>
                            <option v-if="form.sampler && !samplers.includes(form.sampler)" :value="form.sampler">{{ form.sampler }}</option>
                        </select>
                    </label>
                    <label class="field">
                        <span>Scheduler</span>
                        <select v-model="form.scheduler" class="input">
                            <option value="">model default</option>
                            <option v-for="s in schedulers" :key="s" :value="s">{{ s }}</option>
                            <option v-if="form.scheduler && !schedulers.includes(form.scheduler)" :value="form.scheduler">{{ form.scheduler }}</option>
                        </select>
                    </label>
                    <template v-if="form.video">
                        <label class="field"><span>Frames</span><input v-model.number="form.frames" class="input" type="number" min="1" max="4096" /></label>
                        <label class="field"><span>FPS</span><input v-model.number="form.fps" class="input" type="number" min="1" max="240" /></label>
                    </template>
                    <label class="field"><span>Flow shift</span><input v-model.number="form.flowShift" class="input" type="number" min="0" max="1000" step="0.5" placeholder="default" /></label>
                </div>
                <p class="faint small">Empty fields use the model's own defaults.</p>
            </div>

            <p v-if="error" class="status-message status-message--error">{{ error }}</p>
            <footer class="actions">
                <button class="btn-ghost" type="button" @click="emit('close')">Cancel</button>
                <button class="btn" type="submit" :disabled="saving"><span v-if="saving" class="spin" /><Icon v-else name="check" :size="16" />{{ saving ? "Saving…" : "Save variant" }}</button>
            </footer>
        </form>
    </div>
</template>

<style scoped>
.overlay {
    position: fixed;
    inset: 0;
    z-index: 50;
    display: grid;
    place-items: center;
    padding: 20px;
    background: rgba(5, 5, 8, 0.75);
}

.dialog {
    display: grid;
    grid-template-rows: auto minmax(0, 1fr) auto auto;
    gap: 14px;
    width: min(820px, 100%);
    max-height: calc(100dvh - 40px);
    padding: 20px;
}

.head {
    display: flex;
    align-items: flex-start;
    justify-content: space-between;
}

.head p {
    margin: 2px 0 0;
}

.body {
    display: grid;
    gap: 14px;
    overflow-y: auto;
    padding-right: 4px;
}

.guide {
    display: grid;
    gap: 10px;
    padding: 12px 14px;
    background: var(--accent-soft);
    border-color: transparent;
}

.notes {
    display: grid;
    gap: 4px;
    margin: 0;
    padding-left: 18px;
    color: var(--text-2);
    font-size: 13px;
}

.alts {
    display: flex;
    flex-wrap: wrap;
    align-items: center;
    gap: 6px;
}

.alt {
    min-height: 28px;
    font-size: 12px;
}

h3 {
    margin-top: 6px;
    color: var(--text-2);
    font-size: 12px;
    text-transform: uppercase;
    letter-spacing: 0.08em;
}

.row2,
.slots {
    display: grid;
    grid-template-columns: 1fr 1fr;
    gap: 12px;
}

.grid {
    display: grid;
    grid-template-columns: repeat(4, minmax(0, 1fr));
    gap: 12px;
}

.field em {
    font-style: normal;
    font-weight: 500;
    margin-left: 4px;
}

.req {
    color: var(--accent-strong);
}

.tag {
    padding: 0 6px;
    border-radius: 6px;
    background: var(--accent-soft);
    color: var(--accent-strong);
    font-size: 11px;
}

.missing {
    border-color: var(--warn);
}

small,
.small {
    margin: 0;
    font-size: 12px;
}

.warn {
    margin: 0;
    color: var(--warn);
}

.abilities {
    display: grid;
    grid-template-columns: 1fr 1fr;
    gap: 6px 14px;
}

.check {
    display: flex;
    align-items: center;
    gap: 8px;
    color: var(--text-2);
    font-size: 13px;
}

.more {
    justify-self: start;
}

.actions {
    display: flex;
    justify-content: flex-end;
    gap: 8px;
}

@media (max-width: 700px) {
    .row2,
    .slots {
        grid-template-columns: 1fr;
    }

    .grid {
        grid-template-columns: 1fr 1fr;
    }
}
</style>
