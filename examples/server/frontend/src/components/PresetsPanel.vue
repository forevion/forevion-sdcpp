<script setup lang="ts">
import { computed, ref } from "vue";

import Icon from "./Icon.vue";
import PresetEditor from "./PresetEditor.vue";
import { api } from "../lib/api";
import { ask, confirmDanger } from "../lib/confirm";
import { loadCatalog, loadDownloads, recipeReady, state } from "../lib/store";
import type { Recipe } from "../lib/types";

const editing = ref<{ mode: "new" | "edit" | "copy"; base?: Recipe; guidePackage?: string } | null>(null);
const message = ref("");
const error = ref("");
const templateId = ref("");
const fileInput = ref<HTMLInputElement | null>(null);

const mine = computed(() => state.recipes.filter((r) => r.source === "user").sort((a, b) => a.name.localeCompare(b.name)));
const guides = computed(() => (state.downloads?.packages || []).filter((p) => p.guide?.inputs));
const templates = computed(() => state.recipes.filter((r) => r.source !== "user").sort((a, b) => a.name.localeCompare(b.name)));

const slotNames: Record<string, string> = {
    diffusion_model: "Diffusion model",
    model: "Checkpoint",
    t5xxl: "T5 / UMT5 text encoder",
    llm: "LLM text encoder",
    clip_l: "CLIP-L",
    clip_g: "CLIP-G",
    vae: "VAE",
    tae: "Tiny decoder (TAE)",
    tokenizer: "Tokenizer",
};

function fileName(path: string) {
    return path.split("/").pop();
}

function startTemplate() {
    const base = state.recipes.find((r) => r.id === templateId.value);
    if (base) editing.value = { mode: "copy", base };
}

async function remove(recipe: Recipe) {
    const ok = await confirmDanger("Delete this model config?", `"${recipe.name}" (${recipe.file}) will be deleted. Model files are not touched.`, "Delete config");
    if (!ok) return;
    error.value = "";
    try {
        await api.deleteRecipe(recipe.id);
        await loadCatalog();
    } catch (e) {
        error.value = (e as Error).message;
    }
}

function fileSummary(recipe: Recipe) {
    return Object.values(recipe.models || {}).map(fileName).join(" · ");
}

async function backup() {
    error.value = "";
    try {
        const response = await fetch("/sdcpp/v1/config/backup", { cache: "no-store" });
        if (!response.ok) throw new Error(`backup failed (HTTP ${response.status})`);
        const blob = await response.blob();
        const stamp = new Date().toISOString().slice(0, 16).replace(/[-:T]/g, "");
        const link = Object.assign(document.createElement("a"), { href: URL.createObjectURL(blob), download: `sdcpp-config-${stamp}.json` });
        link.click();
        URL.revokeObjectURL(link.href);
        message.value = "Backup saved to your downloads folder.";
    } catch (e) {
        error.value = (e as Error).message;
    }
}

async function restore(event: Event) {
    const input = event.target as HTMLInputElement;
    const file = input.files?.[0];
    input.value = "";
    if (!file) return;
    error.value = "";
    message.value = "";
    let backupData: any;
    try {
        backupData = JSON.parse(await file.text());
    } catch {
        error.value = `${file.name} is not a JSON file.`;
        return;
    }
    if (backupData?.kind !== "sdcpp-studio-config") {
        error.value = `${file.name} is not an sd.cpp Studio config backup.`;
        return;
    }
    const presets: string[] = (backupData.presets || []).map((p: any) => `config  ${p.file}`);
    const packages: string[] = (backupData.packages || []).map((p: any) => `package ${p.file}`);
    const mode = await ask({
        title: "Restore model configs?",
        message: `${file.name} from ${backupData.created ? new Date(backupData.created).toLocaleString() : "an unknown date"} has ${presets.length} config(s) and ${packages.length} download package(s).`,
        details: [...presets, ...packages],
        warning: "Merge keeps your other configs and overwrites ones with the same file name. Replace deletes all your current configs and added packages first. Model files are never touched.",
        choices: [
            { label: "Replace all", value: "replace", danger: true },
            { label: "Merge", value: "merge", primary: true },
        ],
    });
    if (!mode) return;
    try {
        const response = await fetch("/sdcpp/v1/config/restore", {
            method: "POST",
            headers: { "Content-Type": "application/json" },
            body: JSON.stringify({ backup: backupData, mode }),
        });
        const result = await response.json();
        if (!response.ok) {
            const details = (result.errors || []).map((e: any) => `${e.file}: ${e.message}`).join("; ");
            throw new Error(`${result.error || "restore failed"}${details ? ` — ${details}` : ""}`);
        }
        await Promise.all([loadCatalog(), loadDownloads()]);
        message.value = `Restored ${result.written} file(s)${mode === "replace" ? `, removed ${result.removed} old one(s)` : ""}.`;
    } catch (e) {
        error.value = (e as Error).message;
    }
}
</script>

<template>
    <section class="presets">
        <div class="toolbar">
            <button class="btn" type="button" @click="editing = { mode: 'new' }"><Icon name="create" :size="16" />New model config</button>
            <span class="spacer" />
            <button class="btn-secondary" type="button" @click="backup"><Icon name="download" :size="16" />Backup</button>
            <button class="btn-secondary" type="button" @click="fileInput?.click()"><Icon name="reuse" :size="16" />Restore…</button>
            <input ref="fileInput" class="sr-only" type="file" accept=".json,application/json" @change="restore" />
        </div>
        <p v-if="message" class="status-message">{{ message }}</p>
        <p v-if="error" class="status-message status-message--error">{{ error }}</p>

        <h3 class="section">Setup guides</h3>
        <p class="faint small">What each downloadable model needs, input by input. "Create config" fills a new config with these files and settings for you to check and save.</p>
        <div class="cards">
            <article v-for="p in guides" :key="p.id" class="card panel">
                <div class="card-head">
                    <strong>{{ p.name }}</strong>
                    <span v-if="p.installed" class="chip chip--ok"><Icon name="check" :size="14" />Downloaded</span>
                    <span v-else class="chip">Not downloaded</span>
                </div>
                <dl class="inputs">
                    <template v-for="(file, slot) in p.guide!.inputs" :key="slot">
                        <dt>{{ slotNames[String(slot)] || slot }}</dt>
                        <dd class="mono">{{ fileName(file) }}</dd>
                    </template>
                    <template v-for="(file, slot) in p.guide!.alternatives || {}" :key="`alt-${slot}`">
                        <dt class="faint">or {{ slotNames[String(slot)] || slot }}</dt>
                        <dd class="mono faint">{{ fileName(file) }}</dd>
                    </template>
                </dl>
                <ul class="notes">
                    <li v-for="note in p.guide!.notes" :key="note">{{ note }}</li>
                </ul>
                <div class="actions">
                    <button class="btn-secondary" type="button" @click="editing = { mode: 'new', guidePackage: p.id }"><Icon name="create" :size="16" />Create config</button>
                </div>
            </article>
            <p v-if="!guides.length" class="faint">No guides in the download catalog.</p>
        </div>

        <h3 class="section">Your model configs</h3>
        <div v-if="mine.length" class="cards">
            <article v-for="r in mine" :key="r.id" class="card panel">
                <div class="card-head">
                    <strong>{{ r.name }}</strong>
                    <span v-if="recipeReady(r)" class="chip chip--ok"><Icon name="check" :size="14" />Ready</span>
                    <span v-else class="chip chip--danger" title="Some required files are missing">Missing files</span>
                </div>
                <p v-if="r.description" class="desc">{{ r.description }}</p>
                <p class="mono faint files">{{ fileSummary(r) || "no files chosen" }}</p>
                <p class="faint small">
                    {{ r.defaults?.width }}×{{ r.defaults?.height }} · {{ r.defaults?.steps ?? "?" }} steps<template v-if="r.defaults?.video_frames"> · {{ r.defaults.video_frames }} frames</template>
                    · <span class="mono">{{ r.file }}</span>
                </p>
                <div class="actions">
                    <button class="btn-secondary" type="button" @click="editing = { mode: 'edit', base: r }"><Icon name="edit" :size="16" />Edit</button>
                    <button class="btn-ghost" type="button" @click="editing = { mode: 'copy', base: r }"><Icon name="reuse" :size="16" />Copy</button>
                    <button class="btn-ghost btn-danger" type="button" @click="remove(r)"><Icon name="trash" :size="16" />Delete</button>
                </div>
            </article>
        </div>
        <p v-else class="faint">No model configs yet. Create one above, or from a setup guide.</p>

        <details class="advanced">
            <summary>Start from a built-in model type ({{ templates.length }})</summary>
            <div class="template-row">
                <select v-model="templateId" class="input" aria-label="Built-in model type">
                    <option value="">Choose a model type…</option>
                    <option v-for="r in templates" :key="r.id" :value="r.id">{{ r.name }}</option>
                </select>
                <button class="btn-secondary" type="button" :disabled="!templateId" @click="startTemplate">Create config</button>
            </div>
            <p class="faint small">Built-in types list the inputs a model family needs (for example Flux, SD 3.5, Qwen-Image); pick the files yourself.</p>
        </details>

        <PresetEditor
            v-if="editing"
            :mode="editing.mode"
            :base="editing.base"
            :guide-package="editing.guidePackage"
            @close="editing = null"
        />
    </section>
</template>

<style scoped>
.presets {
    display: grid;
    gap: 14px;
}

.toolbar {
    display: flex;
    flex-wrap: wrap;
    align-items: center;
    gap: 8px;
}

.spacer {
    flex: 1;
}

.section {
    margin-top: 6px;
    color: var(--text-2);
    font-size: 12px;
    text-transform: uppercase;
    letter-spacing: 0.08em;
}

.cards {
    display: grid;
    grid-template-columns: repeat(auto-fill, minmax(320px, 1fr));
    gap: 12px;
}

.card {
    display: grid;
    align-content: start;
    gap: 9px;
    padding: 14px;
}

.card-head {
    display: flex;
    align-items: center;
    justify-content: space-between;
    gap: 8px;
}

.inputs {
    display: grid;
    grid-template-columns: auto minmax(0, 1fr);
    gap: 4px 12px;
    margin: 0;
    font-size: 12px;
}

.inputs dt {
    color: var(--text-2);
}

.inputs dd {
    margin: 0;
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
}

.notes {
    display: grid;
    gap: 3px;
    margin: 0;
    padding-left: 16px;
    color: var(--text-3);
    font-size: 12px;
}

.desc,
.files,
.small {
    margin: 0;
    font-size: 12px;
}

.desc {
    color: var(--text-2);
}

.files {
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
}

.actions {
    display: flex;
    flex-wrap: wrap;
    gap: 6px;
    margin-top: 2px;
}

.advanced summary {
    color: var(--text-2);
    font-size: 13px;
    font-weight: 600;
    cursor: pointer;
}

.template-row {
    display: grid;
    grid-template-columns: minmax(0, 1fr) auto;
    gap: 8px;
    margin: 10px 0 6px;
}
</style>
