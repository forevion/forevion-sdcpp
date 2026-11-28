<script setup lang="ts">
import { computed, onBeforeUnmount, onMounted, ref } from "vue";

import Icon from "../components/Icon.vue";
import MediaView from "../components/MediaView.vue";
import {
    activeJobs,
    currentRecipe,
    deleteOutput,
    formatBytes,
    eta,
    formatDuration,
    loadOutputs,
    plan,
    readyRecipes,
    recipeIsVideo,
    remainingSeconds,
    reuse,
    stageLabels,
    state,
    submit,
} from "../lib/store";
import { confirmDanger } from "../lib/confirm";
import type { OutputEntry } from "../lib/types";

const emit = defineEmits<{ (e: "open-create"): void }>();

const filter = ref<"all" | "video" | "image">("all");
const search = ref("");
const open = ref<OutputEntry | null>(null);
const submitting = ref(false);

const items = computed(() => {
    const q = search.value.trim().toLowerCase();
    return state.outputs.filter((o) => {
        const video = o.mime_type.startsWith("video/");
        if (filter.value === "video" && !video) return false;
        if (filter.value === "image" && video) return false;
        return !q || (o.prompt || "").toLowerCase().includes(q);
    });
});

const pending = computed(() => [...activeJobs.value].reverse());

function progressOf(id: string) {
    const job = state.activeJob;
    if (!job || job.id !== id || !job.progress?.total) return null;
    return (job.progress.completed || 0) / job.progress.total;
}

async function generate() {
    if (!state.form.prompt.trim() || submitting.value) return;
    submitting.value = true;
    try {
        await submit();
    } finally {
        submitting.value = false;
    }
}

function reuseAndEdit(output: OutputEntry) {
    reuse(output);
    open.value = null;
    emit("open-create");
}

async function remove(output: OutputEntry) {
    const ok = await confirmDanger("Delete this result?", `${output.file} will be removed from the gallery and from disk. This cannot be undone.`, "Delete");
    if (!ok) return;
    await deleteOutput(output);
    open.value = null;
}

function step(delta: number) {
    if (!open.value) return;
    const index = items.value.findIndex((o) => o.file === open.value!.file);
    const next = items.value[index + delta];
    if (next) {
        open.value = next;
    }
}

function onKey(event: KeyboardEvent) {
    if (!open.value) return;
    if (event.key === "Escape") open.value = null;
    if (event.key === "ArrowRight") step(1);
    if (event.key === "ArrowLeft") step(-1);
}

onMounted(() => window.addEventListener("keydown", onKey));
onBeforeUnmount(() => window.removeEventListener("keydown", onKey));

function when(ms: number) {
    return new Date(ms).toLocaleString(undefined, { dateStyle: "medium", timeStyle: "short" });
}
</script>

<template>
    <div class="gallery">
        <header class="head">
            <h1>Gallery</h1>
            <span class="faint">{{ state.outputs.length }} items</span>
            <span class="spacer" />
            <input v-model="search" class="input search" placeholder="Search prompts" />
            <div class="segmented" role="group" aria-label="Filter">
                <button type="button" :aria-pressed="filter === 'all'" @click="filter = 'all'">All</button>
                <button type="button" :aria-pressed="filter === 'video'" @click="filter = 'video'">Videos</button>
                <button type="button" :aria-pressed="filter === 'image'" @click="filter = 'image'">Images</button>
            </div>
        </header>

        <div class="grid-wrap">
            <p v-if="!state.outputsEnabled" class="status-message">Saving is off on this server (started with --output-dir none).</p>
            <p v-if="state.loading.outputs" class="loading-row"><span class="spin" />Loading the gallery…</p>
            <div v-else-if="items.length || pending.length" class="grid">
                <div v-for="job in pending" :key="job.id" class="tile pending">
                    <div class="pending-body">
                        <div class="ring" />
                        <strong>{{ job.status === "queued" ? `Queued #${job.queue_position}` : stageLabels[job.progress?.stage || ""] || "Working" }}</strong>
                        <span class="muted">{{ eta(remainingSeconds(job)) }}</span>
                        <div v-if="job.status === 'generating'" class="progress" :class="{ 'progress--indeterminate': progressOf(job.id) == null }">
                            <div :style="{ width: `${(progressOf(job.id) ?? 0) * 100}%` }" />
                        </div>
                    </div>
                    <p class="caption">{{ job.summary?.prompt }}</p>
                </div>
                <button v-for="item in items" :key="item.file" type="button" class="tile" @click="open = item">
                    <MediaView :output="item" thumb />
                    <p class="caption">{{ item.prompt }}</p>
                </button>
            </div>
            <div v-else class="empty">
                <Icon name="gallery" :size="34" />
                <p>Nothing here yet. Type a prompt below to make your first clip.</p>
            </div>
        </div>

        <form class="bar panel" @submit.prevent="generate">
            <select v-model="state.form.recipeId" class="input model-select" aria-label="Model">
                <option v-for="r in readyRecipes" :key="r.id" :value="r.id">{{ r.name }}</option>
            </select>
            <div class="segmented mode" role="group" aria-label="Output type">
                <button type="button" title="Text → video" :aria-pressed="state.form.task === 't2v'" :disabled="!recipeIsVideo(currentRecipe)" @click="state.form.task = 't2v'"><Icon name="video" :size="16" /></button>
                <button type="button" title="Text → image" :aria-pressed="state.form.task === 't2i'" @click="state.form.task = 't2i'"><Icon name="image" :size="16" /></button>
            </div>
            <input v-model="state.form.prompt" class="input prompt" placeholder="Describe your video…" />
            <span class="bar-info faint">{{ plan.width }}×{{ plan.height }}<template v-if="plan.video"> · {{ plan.seconds.toFixed(1) }}s</template></span>
            <button class="btn" type="submit" :disabled="!state.form.prompt.trim() || submitting || !readyRecipes.length">
                <span v-if="submitting" class="spin" /><Icon v-else name="sparkles" :size="16" />Go
            </button>
        </form>

        <div v-if="open" class="lightbox" role="dialog" aria-modal="true" @click.self="open = null">
            <div class="lb-card panel">
                <div class="lb-media">
                    <MediaView :output="open" />
                    <button class="btn-ghost icon-btn nav prev" type="button" aria-label="Previous" @click="step(-1)"><Icon name="chevron" style="transform: rotate(180deg)" /></button>
                    <button class="btn-ghost icon-btn nav next" type="button" aria-label="Next" @click="step(1)"><Icon name="chevron" /></button>
                </div>
                <aside class="lb-side">
                    <div class="lb-top">
                        <span class="chip chip--accent">{{ open.mime_type.startsWith("video/") ? "Video" : "Image" }}</span>
                        <span class="spacer" />
                        <button class="btn-ghost icon-btn" type="button" aria-label="Close" @click="open = null"><Icon name="close" /></button>
                    </div>
                    <p class="lb-prompt">{{ open.prompt }}</p>
                    <dl class="facts">
                        <dt>Model</dt>
                        <dd>{{ state.recipes.find((r) => r.id === open!.recipe)?.name || open.recipe }}</dd>
                        <dt>Size</dt>
                        <dd>{{ open.width }}×{{ open.height }}</dd>
                        <template v-if="(open.frame_count || 1) > 1">
                            <dt>Length</dt>
                            <dd>{{ open.frame_count }} frames · {{ open.fps }} fps</dd>
                        </template>
                        <dt>Steps</dt>
                        <dd>{{ open.steps }}</dd>
                        <dt>Seed</dt>
                        <dd class="mono">{{ open.seed }}</dd>
                        <dt>Render time</dt>
                        <dd>{{ formatDuration(open.elapsed_ms / 1000) }}<template v-if="open.device"> on {{ open.device }}</template></dd>
                        <dt>Created</dt>
                        <dd>{{ when(open.created) }}</dd>
                        <dt>File</dt>
                        <dd class="mono">{{ open.file }} · {{ formatBytes(open.bytes) }}</dd>
                    </dl>
                    <div class="lb-actions">
                        <button class="btn" type="button" @click="reuseAndEdit(open)"><Icon name="reuse" :size="16" />Reuse settings</button>
                        <a class="btn-secondary" :href="open.url" :download="open.file"><Icon name="download" :size="16" />Save</a>
                        <button class="btn-ghost btn-danger" type="button" @click="remove(open)"><Icon name="trash" :size="16" />Delete</button>
                    </div>
                </aside>
            </div>
        </div>
    </div>
</template>

<style scoped>
.gallery {
    display: grid;
    grid-template-rows: auto minmax(0, 1fr) auto;
    height: 100%;
}

.head {
    display: flex;
    align-items: center;
    flex-wrap: wrap;
    gap: 12px;
    padding: 18px 22px 12px;
}

.spacer {
    flex: 1;
}

.search {
    width: min(260px, 100%);
}

.grid-wrap {
    overflow-y: auto;
    padding: 4px 22px 22px;
}

.grid {
    display: grid;
    grid-template-columns: repeat(auto-fill, minmax(230px, 1fr));
    gap: 14px;
}

.tile {
    display: grid;
    grid-template-rows: auto auto;
    gap: 0;
    padding: 0;
    border: 1px solid var(--line);
    border-radius: var(--radius);
    background: var(--bg-raised);
    overflow: hidden;
    text-align: left;
    cursor: pointer;
    transition: border-color 0.12s, transform 0.12s;
}

.tile:hover {
    border-color: var(--line-strong);
    transform: translateY(-1px);
}

.tile :deep(.media) {
    aspect-ratio: 16 / 10;
}

.caption {
    margin: 0;
    padding: 9px 11px 11px;
    color: var(--text-2);
    font-size: 12px;
    display: -webkit-box;
    -webkit-line-clamp: 2;
    -webkit-box-orient: vertical;
    overflow: hidden;
}

.pending {
    cursor: default;
    border-style: dashed;
    border-color: var(--accent);
}

.pending-body {
    aspect-ratio: 16 / 10;
    display: grid;
    align-content: center;
    justify-items: center;
    gap: 8px;
    padding: 16px;
    background: var(--accent-soft);
}

.pending-body .progress {
    width: 70%;
}

.ring {
    width: 26px;
    height: 26px;
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

.bar {
    display: flex;
    align-items: center;
    gap: 8px;
    margin: 0 22px 18px;
    padding: 8px;
    box-shadow: 0 10px 40px rgba(0, 0, 0, 0.45);
}

.model-select {
    width: 200px;
}

.mode {
    flex: none;
}

.mode button {
    width: 36px;
}

.prompt {
    flex: 1;
    min-width: 120px;
}

.bar-info {
    font-size: 12px;
    white-space: nowrap;
}

.lightbox {
    position: fixed;
    inset: 0;
    z-index: 40;
    display: grid;
    place-items: center;
    padding: 24px;
    background: rgba(5, 5, 8, 0.82);
    backdrop-filter: blur(6px);
}

.lb-card {
    display: grid;
    grid-template-columns: minmax(0, 1fr) 320px;
    width: min(1400px, 100%);
    max-height: 100%;
    overflow: hidden;
}

.lb-media {
    position: relative;
    display: grid;
    min-height: 300px;
    background: var(--bg-sunken);
}

.lb-media :deep(video),
.lb-media :deep(img) {
    max-height: 82vh;
}

.nav {
    position: absolute;
    top: 50%;
    transform: translateY(-50%);
    background: rgba(0, 0, 0, 0.45);
    color: #fff;
}

.prev {
    left: 10px;
}

.next {
    right: 10px;
}

.lb-side {
    display: flex;
    flex-direction: column;
    gap: 14px;
    padding: 16px;
    border-left: 1px solid var(--line);
    overflow-y: auto;
}

.lb-top {
    display: flex;
    align-items: center;
}

.lb-prompt {
    margin: 0;
    line-height: 1.55;
}

.facts {
    display: grid;
    grid-template-columns: auto 1fr;
    gap: 6px 14px;
    margin: 0;
    font-size: 13px;
}

.facts dt {
    color: var(--text-3);
}

.facts dd {
    margin: 0;
    word-break: break-all;
}

.lb-actions {
    display: grid;
    gap: 8px;
    margin-top: auto;
}

.lb-actions a {
    text-decoration: none;
}

@media (max-width: 900px) {
    .lb-card {
        grid-template-columns: 1fr;
        overflow-y: auto;
    }

    .bar {
        flex-wrap: wrap;
    }

    .model-select {
        width: 100%;
    }
}
</style>
