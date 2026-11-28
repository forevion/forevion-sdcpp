<script setup lang="ts">
import { computed, onMounted, reactive, ref } from "vue";

import Icon from "./Icon.vue";
import { api } from "../lib/api";
import { categoryLabel } from "../lib/categories";
import { navigation } from "../lib/modelActions";
import { formatBytes, loadCatalog, loadDownloads, startPolling, state } from "../lib/store";
import type { ModelSlotDef, Recipe } from "../lib/types";

const emit = defineEmits<{ (e: "close"): void }>();

// Step 1: model type, step 2: a file for each input, step 3: name and save.
const step = ref(1);
const search = ref("");
const type = ref<Recipe | null>(null);
const freeForm = ref(false);
const slotDefs = ref<ModelSlotDef[]>([]);
const choices = reactive<Record<string, string>>({});
const links = reactive<Record<string, string>>({});
const pending = reactive<Record<string, { id: string; path: string; error?: string }>>({});
const name = ref("");
const error = ref("");
const saving = ref(false);
const savedId = ref("");

const COMMON = ["diffusion_model", "model", "t5xxl", "llm", "clip_l", "clip_g", "vae", "tae", "tokenizer"];

const types = computed(() => {
    const q = search.value.trim().toLowerCase();
    return state.recipes
        .filter((r) => r.source === "builtin")
        .filter((r) => !q || r.name.toLowerCase().includes(q) || (r.description || "").toLowerCase().includes(q))
        .sort((a, b) => a.name.localeCompare(b.name));
});

const inputs = computed(() => {
    if (type.value)
        return type.value.slots.map((s) => ({ id: s.id, label: s.label || s.id, required: s.required, categories: s.categories || [s.category] }));
    return slotDefs.value
        .filter((s) => COMMON.includes(s.id))
        .sort((a, b) => COMMON.indexOf(a.id) - COMMON.indexOf(b.id))
        .map((s) => ({ id: s.id, label: s.label, required: false, categories: s.categories }));
});

const missing = computed(() => {
    const result = inputs.value.filter((i) => i.required && !choices[i.id]).map((i) => i.label);
    for (const group of type.value?.required_any || [])
        if (!group.some((id) => choices[id])) result.push(group.map((id) => inputs.value.find((i) => i.id === id)?.label || id).join(" or "));
    if (freeForm.value && !choices.diffusion_model && !choices.model) result.push("Diffusion model (or a Checkpoint)");
    return result;
});
const downloading = computed(() => Object.keys(pending).some((slot) => !pending[slot].error && !fileExists(pending[slot].path)));

function filesFor(categories: string[]) {
    const allowed = new Set(categories);
    return state.files.filter((f) => allowed.has(f.category)).sort((a, b) => (a.name || a.id).localeCompare(b.name || b.id));
}

function fileExists(path: string) {
    return state.files.some((f) => f.id === path);
}

function chooseType(recipe: Recipe | null) {
    type.value = recipe;
    freeForm.value = !recipe;
    for (const key of Object.keys(choices)) delete choices[key];
    name.value = recipe ? `My ${recipe.name}` : "";
    step.value = 2;
}

function packageProgress(id: string) {
    const pkg = state.downloads?.packages.find((p) => p.id === id);
    if (!pkg) return "";
    const s = pkg.status;
    if (s.state === "failed") return `failed: ${s.error}`;
    if (pkg.installed) return "downloaded";
    if (s.state === "downloading" && s.total_bytes) return `${Math.round((s.done_bytes / s.total_bytes) * 100)}% of ${formatBytes(s.total_bytes)}`;
    return s.state === "idle" ? "waiting" : s.state;
}

async function downloadLink(slot: { id: string; categories: string[] }) {
    const url = (links[slot.id] || "").trim();
    if (!url) return;
    pending[slot.id] = { id: "", path: "" };
    try {
        const result = await api.addCustomModel({ url, category: slot.categories[0], start: true });
        pending[slot.id] = { id: result.id || "", path: result.path };
        choices[slot.id] = result.path;
        links[slot.id] = "";
        await loadDownloads();
        startPolling();
    } catch (e) {
        pending[slot.id] = { id: "", path: "", error: (e as Error).message };
    }
}

function slugify(text: string) {
    return text.toLowerCase().replace(/[^a-z0-9]+/g, "-").replace(/^-+|-+$/g, "").slice(0, 100) || "my-model";
}

async function save() {
    error.value = "";
    if (!name.value.trim()) return void (error.value = "Give your model a name.");
    let id = slugify(name.value);
    for (let n = 2; state.recipes.some((r) => r.id === id); n++) id = `${slugify(name.value)}-${n}`;
    const models: Record<string, string> = {};
    for (const [slot, file] of Object.entries(choices)) if (file) models[slot] = file;
    let recipe: Record<string, any>;
    if (type.value) {
        recipe = JSON.parse(JSON.stringify(type.value));
        for (const key of ["source", "file", "root", "effective_features"]) delete recipe[key];
        for (const slot of recipe.slots) {
            delete slot.categories;
            delete slot.category;
        }
    } else {
        recipe = { slots: Object.keys(models).map((slot) => ({ id: slot, required: true })) };
    }
    Object.assign(recipe, { schema_version: 1, id, name: name.value.trim(), models });
    saving.value = true;
    try {
        await api.saveRecipe(recipe);
        await loadCatalog();
        savedId.value = id;
        step.value = 4;
    } catch (e) {
        error.value = (e as Error).message;
    } finally {
        saving.value = false;
    }
}

function useNow() {
    state.form.recipeId = savedId.value;
    navigation.page = "create";
    emit("close");
}

onMounted(async () => {
    try {
        slotDefs.value = (await api.modelSlots()).slots;
    } catch {
        /* the typed path still works */
    }
});
</script>

<template>
    <div class="overlay" role="dialog" aria-modal="true" aria-labelledby="own-title" @click.self="emit('close')">
        <div class="dialog panel">
            <header class="head">
                <div>
                    <h2 id="own-title">Add your own model</h2>
                    <ol class="steps">
                        <li :class="{ on: step === 1, done: step > 1 }">Model type</li>
                        <li :class="{ on: step === 2, done: step > 2 }">Files</li>
                        <li :class="{ on: step >= 3 }">Name</li>
                    </ol>
                </div>
                <button class="btn-ghost icon-btn" type="button" aria-label="Close" @click="emit('close')"><Icon name="close" /></button>
            </header>

            <div class="body">
                <template v-if="step === 1">
                    <p class="muted">Which kind of model is it? This decides which files it needs.</p>
                    <input v-model="search" class="input" placeholder="Search, for example flux, wan, sd 3.5, qwen" autofocus />
                    <div class="types">
                        <button v-for="r in types" :key="r.id" class="type" type="button" @click="chooseType(r)">
                            <strong>{{ r.name }}</strong>
                            <small class="faint">{{ (r.description || "").split(". ")[0] }}</small>
                        </button>
                    </div>
                    <button class="btn-ghost" type="button" @click="chooseType(null)">Not listed: choose the inputs myself</button>
                </template>

                <template v-else-if="step === 2">
                    <p class="muted">
                        For each input, pick a file you already have or paste a download link (Hugging Face links are checked automatically).
                        <template v-if="type">Inputs for {{ type.name }}:</template>
                    </p>
                    <div v-for="slot in inputs" :key="slot.id" class="input-row panel">
                        <div class="input-head">
                            <strong>{{ slot.label }}</strong>
                            <em :class="slot.required ? 'req' : 'faint'">{{ slot.required ? "required" : "optional" }}</em>
                            <span class="faint small">{{ slot.categories.map(categoryLabel).join(" / ") }}</span>
                        </div>
                        <select v-model="choices[slot.id]" class="input">
                            <option value="">{{ slot.required ? "— choose a file —" : "— not used —" }}</option>
                            <option v-if="choices[slot.id] && !fileExists(choices[slot.id])" :value="choices[slot.id]">{{ choices[slot.id].split("/").pop() }} (downloading)</option>
                            <option v-for="f in filesFor(slot.categories)" :key="f.id" :value="f.id">{{ f.name || f.id }}</option>
                        </select>
                        <div class="link-row">
                            <input v-model="links[slot.id]" class="input mono" placeholder="or paste a link to the file" />
                            <button class="btn-secondary" type="button" :disabled="!links[slot.id] || (pending[slot.id] && !pending[slot.id].path && !pending[slot.id].error)" @click="downloadLink(slot)">
                                <span v-if="pending[slot.id] && !pending[slot.id].path && !pending[slot.id].error" class="spin" /><Icon v-else name="download" :size="16" />Download
                            </button>
                        </div>
                        <p v-if="pending[slot.id]?.error" class="err">{{ pending[slot.id].error }}</p>
                        <p v-else-if="pending[slot.id]?.id" class="faint small">{{ pending[slot.id].path.split("/").pop() }}: {{ packageProgress(pending[slot.id].id) }}</p>
                    </div>
                    <p v-if="type?.required_any?.length" class="faint small">
                        Needs at least one of: {{ type.required_any.map((g) => g.map((id) => inputs.find((i) => i.id === id)?.label || id).join(" or ")).join("; ") }}.
                    </p>
                </template>

                <template v-else-if="step === 3">
                    <label class="field">
                        <span>Name</span>
                        <input v-model="name" class="input" placeholder="My Flux model" autofocus />
                    </label>
                    <p class="faint small">It appears on the Models page and in Create. You can change its settings later under Variants.</p>
                    <p v-if="downloading" class="status-message">Some files are still downloading; you can save now, and the model becomes ready when they finish.</p>
                </template>

                <template v-else>
                    <div class="done">
                        <Icon name="check" :size="28" />
                        <h3>{{ name }} is saved</h3>
                        <p class="muted">{{ downloading ? "It becomes ready as soon as its downloads finish." : "It's ready to use." }}</p>
                    </div>
                </template>
            </div>

            <p v-if="error" class="status-message status-message--error">{{ error }}</p>
            <footer class="actions">
                <button v-if="step === 2 || step === 3" class="btn-ghost" type="button" @click="step--">Back</button>
                <span class="spacer" />
                <template v-if="step === 2">
                    <span v-if="missing.length" class="faint small">Still needed: {{ missing.join(", ") }}</span>
                    <button class="btn" type="button" :disabled="missing.length > 0" @click="step = 3">Next</button>
                </template>
                <button v-else-if="step === 3" class="btn" type="button" :disabled="saving" @click="save">
                    <span v-if="saving" class="spin" /><Icon v-else name="check" :size="16" />Save model
                </button>
                <template v-else-if="step === 4">
                    <button class="btn-ghost" type="button" @click="emit('close')">Close</button>
                    <button class="btn" type="button" :disabled="downloading" @click="useNow"><Icon name="play" :size="16" />Use it</button>
                </template>
            </footer>
        </div>
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
    width: min(760px, 100%);
    max-height: 100%;
    padding: 20px;
}

.head {
    display: flex;
    align-items: flex-start;
    justify-content: space-between;
}

.steps {
    display: flex;
    gap: 16px;
    margin: 6px 0 0;
    padding: 0;
    list-style: none;
    color: var(--text-3);
    font-size: 12px;
    font-weight: 600;
}

.steps li.on {
    color: var(--accent-strong);
}

.steps li.done {
    color: var(--text-2);
}

.body {
    display: grid;
    align-content: start;
    gap: 12px;
    overflow-y: auto;
    padding-right: 4px;
}

.body p {
    margin: 0;
}

.types {
    display: grid;
    grid-template-columns: repeat(auto-fill, minmax(220px, 1fr));
    gap: 8px;
    max-height: 380px;
    overflow-y: auto;
}

.type {
    display: grid;
    gap: 3px;
    padding: 10px 12px;
    border: 1px solid var(--line);
    border-radius: 10px;
    background: var(--bg-raised);
    text-align: left;
    cursor: pointer;
}

.type:hover {
    border-color: var(--accent);
}

.type small {
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
}

.input-row {
    display: grid;
    gap: 8px;
    padding: 12px;
}

.input-head {
    display: flex;
    flex-wrap: wrap;
    align-items: baseline;
    gap: 8px;
}

.input-head em {
    font-style: normal;
    font-weight: 500;
    font-size: 12px;
}

.req {
    color: var(--accent-strong);
}

.link-row {
    display: grid;
    grid-template-columns: minmax(0, 1fr) auto;
    gap: 8px;
}

.small {
    font-size: 12px;
}

.err {
    color: var(--danger);
    font-size: 12px;
}

.done {
    display: grid;
    justify-items: center;
    gap: 8px;
    padding: 30px 0;
    color: var(--ok);
    text-align: center;
}

.done h3 {
    color: var(--text);
}

.actions {
    display: flex;
    align-items: center;
    gap: 8px;
}

.spacer {
    flex: 1;
}
</style>
