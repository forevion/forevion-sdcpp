<script setup lang="ts">
import { computed } from "vue";

import Icon from "./Icon.vue";
import { cancel, eta, formatDuration, queueWaitSeconds, remainingSeconds, stageLabels, state } from "../lib/store";

const emit = defineEmits<{ (e: "close"): void }>();

const active = computed(() => state.jobs.filter((j) => j.status === "queued" || j.status === "generating"));
const done = computed(() =>
    state.jobs
        .filter((j) => j.status !== "queued" && j.status !== "generating")
        .sort((a, b) => (b.completed || 0) - (a.completed || 0))
        .slice(0, 10),
);

const totalLeft = computed(() => {
    let sum = 0;
    for (const job of active.value) {
        const r = remainingSeconds(job);
        if (r == null) return null;
        sum += r;
    }
    return sum;
});

function ratio(id: string) {
    const job = state.activeJob;
    return job && job.id === id && job.progress?.total ? (job.progress.completed || 0) / job.progress.total : null;
}

function describe(s: NonNullable<(typeof state.jobs)[number]["summary"]>) {
    const parts = [`${s.width}×${s.height}`];
    if (s.video_frames && s.video_frames > 1) parts.push(`${s.video_frames} frames`);
    else parts.push("image");
    if (s.steps) parts.push(`${s.steps} steps`);
    return parts.join(" · ");
}
</script>

<template>
    <aside class="queue" aria-label="Queue">
        <header class="q-head">
            <h2>Queue</h2>
            <span v-if="active.length" class="chip chip--accent">{{ active.length }} active · {{ eta(totalLeft) }}</span>
            <span class="spacer" />
            <button class="btn-ghost icon-btn" type="button" aria-label="Close queue" @click="emit('close')"><Icon name="close" /></button>
        </header>

        <div class="q-body">
            <p v-if="!active.length" class="faint idle">Nothing running. Jobs you start appear here with progress and time left.</p>
            <article v-for="job in active" :key="job.id" class="job" :class="job.status">
                <div class="job-row">
                    <strong>{{ job.status === "queued" ? `#${job.queue_position} in line` : stageLabels[job.progress?.stage || ""] || job.progress?.stage }}</strong>
                    <span class="spacer" />
                    <button class="btn-ghost icon-btn btn-danger" type="button" :title="job.status === 'queued' ? 'Remove from queue' : 'Stop'" @click="cancel(job.id)">
                        <Icon :name="job.status === 'queued' ? 'close' : 'stop'" :size="16" />
                    </button>
                </div>
                <p class="prompt">{{ job.summary?.prompt }}</p>
                <p v-if="job.summary" class="faint small">{{ state.recipes.find((r) => r.id === job.summary!.recipe)?.name || job.summary.recipe }} · {{ describe(job.summary) }}</p>
                <template v-if="job.status === 'generating'">
                    <div class="progress" :class="{ 'progress--indeterminate': ratio(job.id) == null }">
                        <div :style="{ width: `${(ratio(job.id) ?? 0) * 100}%` }" />
                    </div>
                    <div class="job-row small">
                        <span class="faint">{{ job.progress?.total ? `step ${job.progress.completed}/${job.progress.total}` : "" }}</span>
                        <span class="spacer" />
                        <span class="faint">{{ formatDuration((job.progress?.elapsed_ms || 0) / 1000) }} elapsed</span>
                        <span class="accent">{{ eta(remainingSeconds(job)) + " left" }}</span>
                    </div>
                </template>
                <div v-else class="job-row small">
                    <span class="faint">starts in {{ eta(queueWaitSeconds(job)) }}</span>
                    <span class="spacer" />
                    <span class="faint">takes {{ eta(remainingSeconds(job)) }}</span>
                </div>
            </article>

            <template v-if="done.length">
                <h3 class="faint">Recent</h3>
                <article v-for="job in done" :key="job.id" class="job done">
                    <div class="job-row">
                        <span :class="['chip', job.status === 'completed' ? 'chip--ok' : job.status === 'failed' ? 'chip--danger' : '']">{{ job.status }}</span>
                        <span class="spacer" />
                        <span v-if="job.started && job.completed" class="faint small">{{ formatDuration((job.completed - job.started) / 1000) }}</span>
                    </div>
                    <p class="prompt">{{ job.summary?.prompt }}</p>
                    <p v-if="job.error && job.status === 'failed'" class="status-message status-message--error small">{{ job.error.message }}</p>
                </article>
            </template>
        </div>
    </aside>
</template>

<style scoped>
.queue {
    display: flex;
    flex-direction: column;
    width: 340px;
    height: 100%;
    border-left: 1px solid var(--line);
    background: var(--bg-raised);
}

.q-head {
    display: flex;
    align-items: center;
    gap: 10px;
    padding: 16px;
    border-bottom: 1px solid var(--line);
}

.spacer {
    flex: 1;
}

.q-body {
    display: grid;
    align-content: start;
    gap: 10px;
    padding: 14px;
    overflow-y: auto;
}

.idle {
    margin: 8px 2px;
}

.job {
    display: grid;
    gap: 7px;
    padding: 12px;
    border-radius: 10px;
    border: 1px solid var(--line);
    background: var(--bg);
}

.job.generating {
    border-color: var(--accent);
}

.job-row {
    display: flex;
    align-items: center;
    gap: 8px;
}

.prompt {
    margin: 0;
    color: var(--text-2);
    font-size: 13px;
    display: -webkit-box;
    -webkit-line-clamp: 2;
    -webkit-box-orient: vertical;
    overflow: hidden;
}

.small {
    margin: 0;
    font-size: 12px;
}

.accent {
    color: var(--accent-strong);
}

h3 {
    margin-top: 8px;
    font-size: 12px;
    text-transform: uppercase;
    letter-spacing: 0.08em;
}

@media (max-width: 720px) {
    .queue {
        position: fixed;
        inset: 0 0 56px 0;
        width: auto;
        z-index: 30;
    }
}
</style>
