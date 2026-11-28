<script setup lang="ts">
import { computed, ref, watch } from "vue";

import AddModelDialog from "../components/AddModelDialog.vue";
import CommandDialog from "../components/CommandDialog.vue";
import Icon from "../components/Icon.vue";
import AudioInput from "../components/AudioInput.vue";
import ImageInput from "../components/ImageInput.vue";
import VideoFramesInput from "../components/VideoFramesInput.vue";
import MediaView from "../components/MediaView.vue";
import PresetEditor from "../components/PresetEditor.vue";
import { categoryLabel } from "../lib/categories";
import { confirmDanger } from "../lib/confirm";
import {
    activeJobs,
    addLora,
    currentLoras,
    loadDownloads,
    loraFamilyWarning,
    loraInstalled,
    loraSupport,
    removeLora,
    startPolling,
    cancel,
    chooseFile,
    currentRecipe,
    deleteOutput,
    estimateTotalSeconds,
    eta,
    formatDuration,
    hasFileChanges,
    currentTask,
    inputSupport,
    missingFiles,
    missingInputs,
    tasks,
    newCreation,
    plan,
    readyRecipes,
    recipeIsVideo,
    remainingSeconds,
    resetFileChoices,
    reuse,
    selectedFiles,
    stageLabels,
    state,
    submit,
} from "../lib/store";
import type { Aspect, LoraStage } from "../lib/store";
import { availableTasks, TASK_NOT_IN_SDCPP, TASKS } from "../lib/tasks";
import type { TaskDef, TaskId } from "../lib/tasks";
import type { OutputEntry, Recipe } from "../lib/types";

const emit = defineEmits<{ (e: "open-models"): void; (e: "open-gallery"): void }>();

const form = state.form;
const aspects: Aspect[] = ["16:9", "4:3", "1:1", "9:16"];
const submitting = ref(false);
const savingPreset = ref<Recipe | null>(null);
const filesOpen = ref(false);
const showCommand = ref(false);
const addingLora = ref(false);

const loraChoices = computed(() => {
    const chosen = new Set(currentLoras.value.map((l) => l.path));
    return state.loras.filter((l) => !chosen.has(l.path)).sort((a, b) => (a.display_path || a.path).localeCompare(b.display_path || b.path));
});
const loraStages: { id: LoraStage; label: string }[] = [
    { id: "both", label: "Both" },
    { id: "high", label: "High noise" },
    { id: "low", label: "Low noise" },
];

function loraName(path: string) {
    const file = state.loras.find((l) => l.path === path);
    return file?.display_path || file?.name || path;
}

function onLoraPick(event: Event) {
    const select = event.target as HTMLSelectElement;
    addLora(select.value);
    select.value = "";
}

function onLoraAdded() {
    loadDownloads();
    startPolling();
}

const startImages = computed(() => (state.inputs.startImage ? [{ name: state.inputs.startName, data: state.inputs.startImage }] : []));
const endImages = computed(() => (state.inputs.endImage ? [{ name: state.inputs.endName, data: state.inputs.endImage }] : []));
const hasInputs = computed(() => {
    const s = inputSupport.value;
    return s.startFrame || s.endFrame || s.refImages || s.audio || s.control || s.strength;
});

function setEnd(image: { name: string; data: string }) {
    state.inputs.endImage = image.data;
    state.inputs.endName = image.name;
}

function clearEnd() {
    state.inputs.endImage = "";
    state.inputs.endName = "";
}

function setStart(image: { name: string; data: string }) {
    state.inputs.startImage = image.data;
    state.inputs.startName = image.name;
}

function clearStart() {
    state.inputs.startImage = "";
    state.inputs.startName = "";
}

// One dropdown: ready variants first, then variants with missing files, then model types to set up.
const modelGroups = computed(() => {
    const ready = new Set(readyRecipes.value.map((r) => r.id));
    const rest = state.recipes.filter((r) => !ready.has(r.id)).sort((a, b) => a.name.localeCompare(b.name));
    return { ready: readyRecipes.value, mine: rest.filter((r) => r.source === "user"), builtin: rest.filter((r) => r.source !== "user") };
});
const currentIsReady = computed(() => readyRecipes.value.some((r) => r.id === currentRecipe.value?.id));

function pickModel(event: Event) {
    const id = (event.target as HTMLSelectElement).value;
    if (!id) return;
    form.recipeId = id;
    blocked.value = "";
    if (!readyRecipes.value.some((r) => r.id === id)) filesOpen.value = true;
}

// Every task is shown; ones the chosen model cannot do explain which models can.
const supported = computed(() => new Set(tasks.value.map((t) => t.id)));
const blocked = ref<TaskId | "">("");
const blockedTask = computed(() => TASKS.find((t) => t.id === blocked.value) || null);
const whoCan = computed(() => {
    const task = blocked.value;
    if (!task) return { ready: [] as Recipe[], types: [] as string[] };
    const can = (r: Recipe) => availableTasks(r).some((t) => t.id === task);
    return {
        ready: readyRecipes.value.filter(can),
        types: state.recipes.filter((r) => r.source !== "user" && can(r)).map((r) => r.name).slice(0, 8),
    };
});

function chooseTask(task: TaskDef) {
    if (supported.value.has(task.id)) {
        form.task = task.id;
        blocked.value = "";
    } else blocked.value = blocked.value === task.id ? "" : task.id;
}

function switchTo(recipe: Recipe) {
    const task = blocked.value;
    form.recipeId = recipe.id;
    if (task) form.task = task;
    blocked.value = "";
}

function taskIcon(task: TaskDef) {
    return task.output === "image" ? "image" : task.output === "audio" ? "sparkles" : "video";
}

function filesFor(categories: string[] | undefined) {
    const allowed = new Set(categories || []);
    return state.files.filter((f) => allowed.has(f.category)).sort((a, b) => (a.name || a.id).localeCompare(b.name || b.id));
}

function onFilesToggle(event: Event) {
    filesOpen.value = (event.target as HTMLDetailsElement).open;
}

function onFileChange(slot: string, event: Event) {
    chooseFile(slot, (event.target as HTMLSelectElement).value);
}

function typeText(slot: { categories?: string[] }) {
    return (slot.categories || []).map(categoryLabel).join(" / ");
}

function slotLabel(id: string) {
    return currentRecipe.value?.slots.find((s) => s.id === id)?.label || id;
}

const requiredAnyText = computed(() =>
    (currentRecipe.value?.required_any || []).map((group) => group.map(slotLabel).join(" or ")).join("; "),
);

function saveAsPreset() {
    if (!currentRecipe.value) return;
    const models: Record<string, string> = {};
    for (const [slot, file] of Object.entries(selectedFiles.value)) if (file) models[slot] = file;
    savingPreset.value = { ...currentRecipe.value, models };
}

function onPresetSaved(id: string) {
    form.recipeId = id;
}

const isVideoModel = computed(() => recipeIsVideo(currentRecipe.value));
// Keep the stored task valid for the chosen model, and the legacy mode flag in step with it.
watch(currentTask, (task) => {
    if (task && form.task !== task.id) form.task = task.id;
    form.mode = task?.output === "video" ? "video" : "image";
}, { immediate: true });
watch(isVideoModel, (video) => {
    if (!video) form.mode = "image";
});

const maxSeconds = computed(() => {
    const p = plan.value;
    const max = currentRecipe.value?.video_constraints?.max_frames;
    return Math.min(8, max ? max / p.fps : 8);
});

const estimate = computed(() => {
    const p = plan.value;
    return estimateTotalSeconds({ recipe: p.recipe?.id, width: p.width, height: p.height, steps: p.steps, video_frames: p.renderFrames });
});

const running = computed(() => state.jobs.find((j) => j.status === "generating"));
const progress = computed(() => state.activeJob?.progress);
const progressRatio = computed(() => {
    const p = progress.value;
    return p && p.total ? Math.min(1, (p.completed || 0) / p.total) : null;
});
const preview = computed(() => (state.activeJob?.preview ? `data:image/png;base64,${state.activeJob.preview.b64_json}` : ""));
const shown = computed(() => state.createSelected || state.lastCompleted || null);
const recent = computed(() => state.outputs.slice(0, 14));

watch(
    () => state.lastCompleted,
    () => {
        state.createSelected = null;
    },
);

async function generate() {
    if (!form.prompt.trim() || submitting.value) return;
    submitting.value = true;
    try {
        await submit();
    } finally {
        submitting.value = false;
    }
}

function onKey(event: KeyboardEvent) {
    if ((event.ctrlKey || event.metaKey) && event.key === "Enter") {
        event.preventDefault();
        generate();
    }
}

function randomizeSeed() {
    form.seed = Math.floor(Math.random() * 2 ** 31);
    form.lockSeed = true;
}

async function removeShown() {
    const output = shown.value;
    if (!output) return;
    const ok = await confirmDanger("Delete this result?", `${output.file} will be removed from the gallery and from disk. This cannot be undone.`, "Delete");
    if (ok) await deleteOutput(output);
}

function resetAdvanced() {
    form.steps = null;
    form.cfg = null;
    form.fps = null;
    form.negativePrompt = "";
    form.lockSeed = false;
}
</script>

<template>
    <div class="create" @keydown="onKey">
        <section class="form" aria-label="Generation settings">
            <header class="form-head">
                <h1>Create</h1>
                <span class="faint">Ctrl + Enter to generate</span>
                <button class="btn-secondary new-btn" type="button" title="Clear the prompt and preview" @click="newCreation"><Icon name="create" :size="16" />New</button>
            </header>

            <p v-if="state.system?.gpu_build && !state.system.gpu_found" class="status-message status-message--error">
                This is a {{ state.system.gpu_build.toUpperCase() }} build, but no GPU was found, so jobs run on the CPU and are much slower. Check that the
                NVIDIA driver works (<code>nvidia-smi</code>), then restart the server.
            </p>
            <p v-if="state.loading.catalog" class="loading-row"><span class="spin" />Loading models…</p>
            <div v-else-if="!state.files.length" class="panel no-models">
                <Icon name="models" :size="28" />
                <p>No models are ready yet.</p>
                <button class="btn" type="button" @click="emit('open-models')"><Icon name="download" />Get models</button>
            </div>

            <template v-else>
                <div class="field">
                    <span class="label-row">Model <button class="link" type="button" @click="emit('open-models')">Get more models…</button></span>
                    <select class="input model-select" aria-label="Model" :value="currentRecipe?.id || ''" @change="pickModel">
                        <option v-if="!currentRecipe" value="">Choose a model…</option>
                        <optgroup label="Ready to use">
                            <option v-for="r in modelGroups.ready" :key="r.id" :value="r.id">{{ r.name }}</option>
                        </optgroup>
                        <optgroup v-if="modelGroups.mine.length" label="Your variants (files missing)">
                            <option v-for="r in modelGroups.mine" :key="r.id" :value="r.id">{{ r.name }}</option>
                        </optgroup>
                        <optgroup label="Other model types (pick their files yourself)">
                            <option v-for="r in modelGroups.builtin" :key="r.id" :value="r.id">{{ r.name }}</option>
                        </optgroup>
                    </select>
                    <small v-if="currentRecipe" class="faint">
                        <template v-if="!currentIsReady">Needs its files picked under Model files · </template>
                        {{ currentRecipe.defaults?.steps ?? "?" }} steps · {{ currentRecipe.defaults?.width }}×{{ currentRecipe.defaults?.height }}
                    </small>
                </div>

                <div class="field">
                    <span>What to make</span>
                    <div class="task-chips" role="group" aria-label="Task">
                        <button
                            v-for="task in TASKS"
                            :key="task.id"
                            type="button"
                            class="task-chip"
                            :class="{ off: !supported.has(task.id) }"
                            :aria-pressed="supported.has(task.id) ? currentTask?.id === task.id : blocked === task.id"
                            :title="supported.has(task.id) ? task.hint : `${currentRecipe?.name || 'This model'} cannot do this. Click to see which models can.`"
                            @click="chooseTask(task)"
                        >
                            <Icon :name="taskIcon(task)" :size="14" />{{ task.label }}
                        </button>
                    </div>
                    <div v-if="blockedTask" class="blocked">
                        <p v-if="TASK_NOT_IN_SDCPP[blockedTask.id] && !whoCan.types.length">{{ TASK_NOT_IN_SDCPP[blockedTask.id] }}</p>
                        <template v-else>
                            <p><strong>{{ currentRecipe?.name || "This model" }}</strong> cannot do {{ blockedTask.label.toLowerCase() }}. {{ blockedTask.hint }}</p>
                            <div v-if="whoCan.ready.length" class="blocked-actions">
                                <span class="faint small">Ready on this computer:</span>
                                <button v-for="r in whoCan.ready" :key="r.id" class="btn-secondary" type="button" @click="switchTo(r)">Use {{ r.name }}</button>
                            </div>
                            <p v-if="whoCan.types.length" class="faint small">
                                Model types that can: {{ whoCan.types.join(", ") }}. Download one or add your own on
                                <button class="link" type="button" @click="emit('open-models')">Models</button>.
                            </p>
                        </template>
                    </div>
                    <small v-else class="faint">{{ currentTask?.hint }}<template v-if="inputSupport.sound"> This model also makes sound.</template></small>
                </div>

                <details class="files-box" :open="filesOpen || missingFiles.length > 0" @toggle="onFilesToggle">
                    <summary>
                        Model files
                        <span v-if="missingFiles.length" class="chip chip--danger">{{ missingFiles.length }} missing</span>
                        <span v-else-if="hasFileChanges" class="chip chip--accent">changed</span>
                        <span v-else class="faint small">from the variant</span>
                        <Icon name="chevron" :size="14" />
                    </summary>
                    <div class="files-body">
                        <label v-for="slot in currentRecipe?.slots || []" :key="slot.id" class="field">
                            <span>
                                {{ slot.label || slot.id }}
                                <em :class="slot.required ? 'req' : 'faint'">{{ slot.required ? "required" : "optional" }}</em>
                                <em v-if="typeText(slot) !== (slot.label || slot.id)" class="faint">{{ typeText(slot) }}</em>
                            </span>
                            <select class="input" :value="selectedFiles[slot.id] || ''" @change="onFileChange(slot.id, $event)">
                                <option value="">{{ slot.required ? "— choose a file —" : "— none —" }}</option>
                                <option v-for="f in filesFor(slot.categories)" :key="f.id" :value="f.id">{{ f.name || f.id }}</option>
                            </select>
                        </label>
                        <p v-if="requiredAnyText" class="faint small">Needs at least one of: {{ requiredAnyText }}.</p>
                        <div class="files-actions">
                            <button class="link" type="button" :disabled="!hasFileChanges" @click="resetFileChoices">Use the variant's files</button>
                            <button class="link" type="button" :disabled="missingFiles.length > 0" @click="saveAsPreset">Save as variant…</button>
                        </div>
                    </div>
                </details>

                <label class="field">
                    <span>Prompt</span>
                    <textarea
                        v-model="form.prompt"
                        class="input"
                        rows="5"
                        placeholder="A red fox trotting through fresh snow at sunrise, soft golden light, cinematic"
                    />
                </label>

                <section v-if="hasInputs" class="files-box inputs-box">
                    <div class="inputs-head">Inputs <span class="faint small">for {{ currentTask?.label.toLowerCase() }}</span></div>
                    <div class="files-body">
                        <div v-if="inputSupport.startFrame" class="field">
                            <span>{{ inputSupport.startLabel }} <em :class="inputSupport.startRequired ? 'req' : 'faint'">{{ inputSupport.startRequired ? "required" : "optional" }}</em></span>
                            <ImageInput :images="startImages" @add="setStart" @remove="clearStart" />
                            <small v-if="state.inputs.startImage" class="faint">Resized to {{ plan.width }}×{{ plan.height }}; pick a matching shape to avoid cropping.</small>
                        </div>
                        <div v-if="inputSupport.endFrame" class="field">
                            <span>End frame <em class="faint">optional, the clip ends on this picture</em></span>
                            <ImageInput :images="endImages" @add="setEnd" @remove="clearEnd" />
                        </div>
                        <div v-if="inputSupport.refImages" class="field">
                            <span>{{ currentTask?.id === "edit" ? "Images to edit" : "Reference images" }} <em class="faint">{{ currentTask?.id === "edit" ? "describe the change in the prompt" : "refer to them as Picture 1, Picture 2 … in the prompt" }}</em></span>
                            <ImageInput :images="state.inputs.refImages" multiple :max="4" @add="state.inputs.refImages.push($event)" @remove="state.inputs.refImages.splice($event, 1)" />
                        </div>
                        <div v-if="inputSupport.audio" class="field">
                            <span>{{ currentTask?.id === "s2v" ? "Speech" : "Reference audio" }} <em :class="currentTask?.id === 's2v' ? 'req' : 'faint'">{{ currentTask?.id === "s2v" ? "required" : "optional" }}</em></span>
                            <AudioInput :audio="state.inputs.audio" @set="state.inputs.audio = $event" />
                            <small v-if="currentTask?.id === 's2v' && state.inputs.audio && plan.seconds < state.inputs.audio.seconds" class="faint">
                                The clip is {{ plan.seconds.toFixed(1) }} s; set Length to {{ state.inputs.audio.seconds.toFixed(1) }} s to cover all of the speech.
                            </small>
                        </div>
                        <div v-if="inputSupport.control" class="field">
                            <span>Control video <em class="req">required</em></span>
                            <VideoFramesInput :video="state.inputs.control" :frames="plan.renderFrames" :fps="plan.fps" :width="plan.width" :height="plan.height" @set="state.inputs.control = $event" />
                            <small class="faint">Frames are taken at {{ plan.fps }} fps for {{ plan.renderFrames }} frames; set Length and Shape first.</small>
                        </div>
                        <div v-if="inputSupport.strength" class="field">
                            <span>Strength <b class="value">{{ form.strength.toFixed(2) }}</b></span>
                            <input v-model.number="form.strength" class="range" type="range" min="0.05" max="1" step="0.05" />
                            <small class="faint">Low keeps the picture, high changes more.</small>
                        </div>
                    </div>
                </section>

                <div class="row2">
                    <div class="field">
                        <span>Shape</span>
                        <div class="segmented" role="group" aria-label="Aspect ratio">
                            <button v-for="a in aspects" :key="a" type="button" :aria-pressed="form.aspect === a" @click="form.aspect = a">{{ a }}</button>
                        </div>
                    </div>
                    <div class="field">
                        <span>Size</span>
                        <div class="segmented" role="group" aria-label="Size">
                            <button type="button" :aria-pressed="form.size === 'small'" @click="form.size = 'small'">Small</button>
                            <button type="button" :aria-pressed="form.size === 'normal'" @click="form.size = 'normal'">Normal</button>
                        </div>
                    </div>
                </div>

                <div v-if="plan.video" class="field">
                    <span>Length <b class="value">{{ plan.seconds.toFixed(1) }} s · {{ plan.frames }} frames</b></span>
                    <input v-model.number="form.seconds" class="range" type="range" min="0.5" :max="maxSeconds" step="0.5" />
                </div>

                <details v-if="loraSupport.enabled" class="advanced loras">
                    <summary>
                        LoRAs
                        <span v-if="currentLoras.length" class="chip chip--accent">{{ currentLoras.length }}</span>
                        <Icon name="chevron" :size="14" />
                    </summary>
                    <div class="advanced-body">
                        <div v-for="(lora, index) in currentLoras" :key="lora.path" class="lora">
                            <div class="lora-head">
                                <span class="mono ellipsis" :title="lora.path">{{ loraName(lora.path) }}</span>
                                <button class="btn-ghost icon-btn" type="button" :aria-label="`Remove ${loraName(lora.path)}`" title="Remove" @click="removeLora(index)"><Icon name="close" :size="14" /></button>
                            </div>
                            <p v-if="!loraInstalled(lora.path)" class="status-message status-message--error small">This file is not in the LoRA folder any more; it will be skipped.</p>
                            <p v-else-if="loraFamilyWarning(lora.path)" class="warn small">{{ loraFamilyWarning(lora.path) }} It may do nothing or break the result.</p>
                            <label class="field">
                                <span>Strength <b class="value">{{ lora.strength.toFixed(2) }}</b></span>
                                <input v-model.number="lora.strength" class="range" type="range" min="-1" max="2" step="0.05" :aria-label="`Strength of ${loraName(lora.path)}`" />
                            </label>
                            <div v-if="loraSupport.stages" class="field">
                                <span>Applies to</span>
                                <div class="segmented" role="group" aria-label="Which expert the LoRA changes">
                                    <button v-for="stage in loraStages" :key="stage.id" type="button" :aria-pressed="lora.stage === stage.id" @click="lora.stage = stage.id">{{ stage.label }}</button>
                                </div>
                            </div>
                        </div>
                        <select v-if="loraChoices.length" class="input" aria-label="Add a LoRA" value="" @change="onLoraPick">
                            <option value="">+ Add a LoRA…</option>
                            <option v-for="l in loraChoices" :key="l.path" :value="l.path">{{ l.display_path || l.path }}</option>
                        </select>
                        <p v-else-if="!state.loras.length" class="faint small">No LoRA files yet. Put .safetensors files in the <span class="mono">loras</span> folder of your models folder, or add one from a link.</p>
                        <button class="link" type="button" @click="addingLora = true"><Icon name="download" :size="14" />Add LoRA from a link…</button>
                    </div>
                </details>

                <details class="advanced">
                    <summary>Advanced <Icon name="chevron" :size="14" /></summary>
                    <div class="advanced-body">
                        <label class="field">
                            <span>Avoid (negative prompt)</span>
                            <input v-model="form.negativePrompt" class="input" placeholder="blurry, low quality" />
                        </label>
                        <div class="row3">
                            <label class="field">
                                <span>Steps</span>
                                <input v-model.number="form.steps" class="input" type="number" min="1" max="100" :placeholder="String(currentRecipe?.defaults?.steps ?? '')" />
                            </label>
                            <label class="field">
                                <span>CFG</span>
                                <input v-model.number="form.cfg" class="input" type="number" min="0" max="30" step="0.5" :placeholder="String(currentRecipe?.defaults?.cfg_scale ?? '')" />
                            </label>
                            <label class="field">
                                <span>FPS</span>
                                <input v-model.number="form.fps" class="input" type="number" min="1" max="60" :placeholder="String(currentRecipe?.defaults?.fps ?? 24)" :disabled="!plan.video" />
                            </label>
                        </div>
                        <div class="field">
                            <span>Seed</span>
                            <div class="seed">
                                <input v-model.number="form.seed" class="input mono" type="number" :disabled="!form.lockSeed" :placeholder="form.lockSeed ? '' : 'random'" />
                                <button class="btn-secondary icon-btn" type="button" title="New random seed" @click="randomizeSeed"><Icon name="dice" /></button>
                                <button class="btn-secondary icon-btn" type="button" :aria-pressed="form.lockSeed" :title="form.lockSeed ? 'Seed is fixed' : 'Seed is random'" @click="form.lockSeed = !form.lockSeed">
                                    <Icon name="lock" />
                                </button>
                            </div>
                        </div>
                        <button class="link" type="button" @click="resetAdvanced">Reset to model defaults</button>
                    </div>
                </details>

                <div class="go">
                    <div class="summary">
                        <span>{{ plan.width }}×{{ plan.height }}</span>
                        <span v-if="plan.video">{{ plan.frames }} frames @ {{ plan.fps }} fps</span>
                        <span v-else>still image</span>
                        <span>{{ plan.steps }} steps</span>
                        <span v-if="estimate != null" class="accent">{{ eta(estimate) }}</span>
                    </div>
                    <p v-if="missingFiles.length" class="status-message status-message--error">Choose a file for: {{ missingFiles.join(", ") }} (Model files above).</p>
                    <p v-if="missingInputs.length" class="status-message status-message--error">Add {{ missingInputs.join(" and ") }} (Inputs above).</p>
                    <p v-if="state.error" class="status-message status-message--error">{{ state.error }}</p>
                    <div class="go-row">
                        <button class="btn btn-lg" type="button" :disabled="!form.prompt.trim() || submitting || missingFiles.length > 0 || missingInputs.length > 0" @click="generate">
                            <span v-if="submitting" class="spin" aria-hidden="true" />
                            <Icon v-else name="sparkles" />
                            {{ submitting ? "Sending…" : activeJobs.length ? `Add to queue (${activeJobs.length} ahead)` : "Generate" }}
                        </button>
                        <button class="btn-secondary btn-lg" type="button" title="Show the equivalent sd-cli command and API request" @click="showCommand = true">
                            <Icon name="scripts" :size="18" />Command
                        </button>
                    </div>
                </div>
            </template>
            <CommandDialog v-if="showCommand" @close="showCommand = false" />
            <AddModelDialog v-if="addingLora" category="loras" @close="addingLora = false" @added="onLoraAdded" />
            <PresetEditor v-if="savingPreset" :base="savingPreset" mode="copy" @close="savingPreset = null" @saved="onPresetSaved" />
        </section>

        <section class="stage" aria-label="Preview">
            <div class="viewer panel">
                <template v-if="running">
                    <div class="live">
                        <img v-if="preview" :src="preview" alt="Live preview" class="live-img" />
                        <div v-else class="live-placeholder">
                            <div class="spinner" />
                        </div>
                    </div>
                    <div class="live-info">
                        <div class="live-row">
                            <strong>{{ stageLabels[progress?.stage || ""] || progress?.stage || "Starting" }}</strong>
                            <span v-if="progress?.total" class="muted">step {{ progress.completed }} / {{ progress.total }}</span>
                            <span class="spacer" />
                            <span class="muted">{{ formatDuration((progress?.elapsed_ms || 0) / 1000) }} elapsed</span>
                            <span class="accent">{{ eta(remainingSeconds(running)) + " left" }}</span>
                            <button class="btn-ghost btn-danger" type="button" @click="cancel(running.id)"><Icon name="stop" :size="16" />Stop</button>
                        </div>
                        <div class="progress" :class="{ 'progress--indeterminate': progressRatio == null }">
                            <div :style="{ width: `${(progressRatio ?? 0) * 100}%` }" />
                        </div>
                        <p class="prompt-line">{{ running.summary?.prompt }}</p>
                    </div>
                </template>
                <template v-else-if="shown">
                    <MediaView :output="shown" class="result" />
                    <div class="result-info">
                        <p class="prompt-line">{{ shown.prompt }}</p>
                        <div class="result-meta">
                            <span class="chip">{{ shown.width }}×{{ shown.height }}</span>
                            <span v-if="(shown.frame_count || 1) > 1" class="chip">{{ shown.frame_count }} frames</span>
                            <span class="chip">{{ formatDuration(shown.elapsed_ms / 1000) }}</span>
                            <span class="spacer" />
                            <button class="btn-ghost" type="button" @click="reuse(shown)"><Icon name="reuse" :size="16" />Reuse</button>
                            <a class="btn-ghost" :href="shown.url" :download="shown.file"><Icon name="download" :size="16" />Save</a>
                            <button class="btn-ghost btn-danger" type="button" @click="removeShown"><Icon name="trash" :size="16" />Delete</button>
                            <button class="btn-ghost icon-btn" type="button" title="Close preview" aria-label="Close preview" @click="state.createSelected = null; state.lastCompleted = null"><Icon name="close" :size="16" /></button>
                        </div>
                    </div>
                </template>
                <div v-else class="empty">
                    <Icon name="sparkles" :size="34" />
                    <p>Describe a scene and press Generate.<br />Results appear here and in the Gallery.</p>
                </div>
            </div>

            <div v-if="recent.length" class="recent">
                <div class="recent-head">
                    <h2>Recent</h2>
                    <button class="link" type="button" @click="emit('open-gallery')">Open gallery</button>
                </div>
                <div class="strip">
                    <button
                        v-for="item in recent"
                        :key="item.file"
                        type="button"
                        class="thumb"
                        :aria-pressed="shown?.file === item.file"
                        :title="item.prompt"
                        @click="state.createSelected = item"
                    >
                        <MediaView :output="item" thumb />
                    </button>
                </div>
            </div>
        </section>
    </div>
</template>

<style scoped>
.create {
    display: grid;
    grid-template-columns: minmax(320px, 390px) minmax(0, 1fr);
    height: 100%;
}

.form {
    display: flex;
    flex-direction: column;
    gap: 18px;
    padding: 22px 22px 28px;
    border-right: 1px solid var(--line);
    overflow-y: auto;
}

.form-head {
    display: flex;
    align-items: center;
    gap: 12px;
}

.form-head .faint {
    margin-left: auto;
}

.form-head .new-btn {
    min-height: 30px;
    padding: 0 10px;
    font-size: 13px;
}

.form-head .faint {
    font-size: 12px;
}

.no-models {
    display: grid;
    justify-items: center;
    gap: 10px;
    padding: 28px;
    color: var(--text-2);
}

.label-row {
    display: flex;
    align-items: center;
    justify-content: space-between;
    gap: 10px;
}

.model-select {
    font-weight: 600;
}

.blocked {
    display: grid;
    gap: 8px;
    padding: 10px 12px;
    border: 1px dashed var(--line-strong);
    border-radius: 10px;
    background: var(--bg-sunken);
    font-size: 13px;
}

.blocked p {
    margin: 0;
    line-height: 1.5;
}

.blocked-actions {
    display: flex;
    flex-wrap: wrap;
    align-items: center;
    gap: 6px;
}

.blocked .link {
    display: inline;
    font-size: inherit;
}

.small {
    font-size: 12px;
}

.link {
    justify-self: start;
    padding: 0;
    border: 0;
    background: none;
    color: var(--accent-strong);
    font-size: 12px;
    font-weight: 600;
    cursor: pointer;
}

.row2 {
    display: grid;
    grid-template-columns: 1.6fr 1fr;
    gap: 12px;
}

.row3 {
    display: grid;
    grid-template-columns: repeat(3, 1fr);
    gap: 10px;
}

.segmented button {
    display: inline-flex;
    align-items: center;
    justify-content: center;
    gap: 6px;
}

.value {
    float: right;
    color: var(--text);
    font-weight: 600;
}

.task-chips {
    display: flex;
    flex-wrap: wrap;
    gap: 6px;
}

.task-chip {
    display: inline-flex;
    align-items: center;
    gap: 6px;
    min-height: 32px;
    padding: 0 11px;
    border: 1px solid var(--line);
    border-radius: 999px;
    background: var(--bg-sunken);
    color: var(--text-2);
    font-weight: 550;
    font-size: 13px;
    cursor: pointer;
}

.task-chip:hover {
    border-color: var(--line-strong);
    color: var(--text);
}

.task-chip[aria-pressed="true"] {
    border-color: var(--accent);
    background: var(--accent-soft);
    color: var(--text);
}

.task-chip.off {
    border-style: dashed;
    background: transparent;
    color: var(--text-3);
}

.task-chip.off[aria-pressed="true"] {
    border-color: var(--line-strong);
    background: var(--bg-hover);
    color: var(--text-2);
}

.inputs-head {
    font-weight: 600;
    font-size: 13px;
    color: var(--text-2);
}

.inputs-box .req {
    color: var(--accent-strong);
}

.go-row {
    display: grid;
    grid-template-columns: minmax(0, 1fr) auto;
    gap: 8px;
}

.images-box em {
    font-style: normal;
    font-weight: 400;
    margin-left: 4px;
}

.files-box {
    border: 1px solid var(--line);
    border-radius: 10px;
    padding: 10px 12px;
    background: var(--bg-raised);
}

.files-box summary {
    display: flex;
    align-items: center;
    gap: 8px;
    font-weight: 600;
    font-size: 13px;
    color: var(--text-2);
    cursor: pointer;
    list-style: none;
}

.files-box summary::-webkit-details-marker {
    display: none;
}

.files-box summary svg {
    margin-left: auto;
}

.files-box[open] summary svg {
    transform: rotate(90deg);
}

.files-body {
    display: grid;
    gap: 12px;
    margin-top: 12px;
}

.files-body em {
    font-style: normal;
    font-weight: 500;
    margin-left: 4px;
}

.files-body .req {
    color: var(--accent-strong);
}

.files-actions {
    display: flex;
    justify-content: space-between;
    gap: 10px;
}

.small {
    font-size: 12px;
    margin: 0;
}

.advanced {
    border-top: 1px solid var(--line);
    padding-top: 12px;
}

.advanced summary {
    display: flex;
    align-items: center;
    gap: 6px;
    color: var(--text-2);
    font-weight: 600;
    font-size: 13px;
    cursor: pointer;
    list-style: none;
}

.advanced summary::-webkit-details-marker {
    display: none;
}

.advanced[open] summary svg {
    transform: rotate(90deg);
}

.lora {
    display: grid;
    gap: 8px;
    padding: 10px 12px;
    border: 1px solid var(--line);
    border-radius: 10px;
    background: var(--bg-raised);
}

.lora-head {
    display: flex;
    align-items: center;
    gap: 8px;
    min-width: 0;
}

.lora-head .ellipsis {
    flex: 1;
    min-width: 0;
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
    font-size: 13px;
}

.lora .warn {
    margin: 0;
    color: var(--warn);
}

.loras .link {
    display: inline-flex;
    align-items: center;
    gap: 6px;
    justify-self: start;
}

.advanced-body {
    display: grid;
    gap: 14px;
    margin-top: 14px;
}

.seed {
    display: grid;
    grid-template-columns: 1fr auto auto;
    gap: 6px;
}

.seed button[aria-pressed="true"] {
    color: var(--accent-strong);
    border-color: var(--accent);
}

.go {
    display: grid;
    gap: 10px;
    margin-top: auto;
    padding-top: 6px;
}

.summary {
    display: flex;
    flex-wrap: wrap;
    gap: 4px 12px;
    color: var(--text-3);
    font-size: 12px;
}

.accent {
    color: var(--accent-strong);
}

.stage {
    display: flex;
    flex-direction: column;
    gap: 16px;
    min-width: 0;
    padding: 22px;
    overflow-y: auto;
}

.viewer {
    display: flex;
    flex-direction: column;
    flex: 1;
    min-height: 360px;
    overflow: hidden;
}

.live,
.result {
    flex: 1;
    min-height: 0;
    display: grid;
    place-items: center;
    background: var(--bg-sunken);
}

.live-img {
    max-width: 100%;
    max-height: 62vh;
    image-rendering: auto;
    filter: saturate(0.9);
}

.live-placeholder {
    display: grid;
    place-items: center;
    width: 100%;
    height: 100%;
    min-height: 280px;
}

.spinner {
    width: 34px;
    height: 34px;
    border-radius: 50%;
    border: 3px solid var(--line-strong);
    border-top-color: var(--accent);
    animation: spin 0.9s linear infinite;
}

@keyframes spin {
    to {
        transform: rotate(360deg);
    }
}

.live-info,
.result-info {
    display: grid;
    gap: 10px;
    padding: 14px 16px;
    border-top: 1px solid var(--line);
}

.live-row,
.result-meta {
    display: flex;
    align-items: center;
    flex-wrap: wrap;
    gap: 6px 12px;
}

.spacer {
    flex: 1;
}

.prompt-line {
    margin: 0;
    color: var(--text-2);
    display: -webkit-box;
    -webkit-line-clamp: 2;
    -webkit-box-orient: vertical;
    overflow: hidden;
}

.result-meta a {
    text-decoration: none;
}

.recent {
    display: grid;
    gap: 10px;
}

.recent-head {
    display: flex;
    align-items: center;
    justify-content: space-between;
}

.strip {
    display: grid;
    grid-auto-flow: column;
    grid-auto-columns: 132px;
    gap: 8px;
    overflow-x: auto;
    padding-bottom: 6px;
}

.thumb {
    aspect-ratio: 16 / 10;
    padding: 0;
    border-radius: 9px;
    border: 2px solid transparent;
    background: var(--bg-raised);
    overflow: hidden;
    cursor: pointer;
}

.thumb[aria-pressed="true"] {
    border-color: var(--accent);
}

@media (max-width: 980px) {
    .create {
        grid-template-columns: 1fr;
        height: auto;
    }

    .form {
        border-right: 0;
        border-bottom: 1px solid var(--line);
    }
}
</style>
