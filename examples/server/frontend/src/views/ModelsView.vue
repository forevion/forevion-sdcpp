<script setup lang="ts">
import { computed, onMounted, ref } from "vue";

import Icon from "../components/Icon.vue";
import { api } from "../lib/api";
import { eta, formatBytes, loadCatalog, loadDownloads, startPolling, state } from "../lib/store";
import type { DownloadPackage } from "../lib/types";

const expanded = ref<Record<string, boolean>>({});
const busy = ref<Record<string, boolean>>({});

const groups = computed(() => {
    const map = new Map<string, DownloadPackage[]>();
    for (const p of state.downloads?.packages || []) {
        const key = p.group || "Other";
        map.set(key, [...(map.get(key) || []), p]);
    }
    return [...map.entries()];
});

const active = (p: DownloadPackage) => ["queued", "starting", "downloading", "verifying", "packing"].includes(p.status.state);

function ratio(p: DownloadPackage) {
    return p.status.total_bytes ? Math.min(1, p.status.done_bytes / p.status.total_bytes) : 0;
}

function downloadSeconds(p: DownloadPackage) {
    const left = p.status.total_bytes - p.status.done_bytes;
    return p.status.speed_bps > 1024 ? left / p.status.speed_bps : null;
}

const stateText: Record<string, string> = {
    queued: "Queued",
    starting: "Starting",
    downloading: "Downloading",
    verifying: "Checking file",
    packing: "Packing to GGUF (a few minutes, no progress bar)",
};

async function start(p: DownloadPackage) {
    busy.value[p.id] = true;
    try {
        await api.startDownload(p.id);
        await loadDownloads();
        startPolling();
    } finally {
        busy.value[p.id] = false;
    }
}

async function stop(p: DownloadPackage) {
    await api.cancelDownload(p.id);
    await loadDownloads();
}

async function rescan() {
    await loadCatalog(true);
    await loadDownloads();
}

onMounted(loadDownloads);
</script>

<template>
    <div class="models-page">
        <header class="head">
            <div>
                <h1>Models</h1>
                <p class="muted">Download a model once; it then appears under Create. Files go to <span class="mono">{{ state.downloads?.model_root }}</span>.</p>
            </div>
            <span class="spacer" />
            <span v-if="state.downloads" class="chip">{{ Math.round(state.downloads.ram_gb) }} GB RAM</span>
            <button class="btn-secondary" type="button" @click="rescan"><Icon name="refresh" :size="16" />Rescan folder</button>
        </header>

        <p v-for="e in state.downloads?.errors || []" :key="e.file + e.message" class="status-message status-message--error">{{ e.file }}: {{ e.message }}</p>

        <section v-for="[group, packages] in groups" :key="group" class="group">
            <h2>{{ group }}</h2>
            <div class="cards">
                <article v-for="p in packages" :key="p.id" class="card panel" :class="{ installed: p.installed, active: active(p) }">
                    <div class="card-head">
                        <h3>{{ p.name }}</h3>
                        <span v-if="p.installed" class="chip chip--ok"><Icon name="check" :size="14" />Ready</span>
                        <span v-else-if="p.status.state === 'failed'" class="chip chip--danger">Failed</span>
                        <span v-else-if="active(p)" class="chip chip--accent">{{ p.status.state === 'queued' ? `Queued #${p.status.queue_position}` : "Working" }}</span>
                    </div>
                    <p class="desc">{{ p.description }}</p>
                    <div class="facts">
                        <span>{{ formatBytes(p.size) }} total</span>
                        <span v-if="!p.installed && p.missing_bytes !== p.size">{{ formatBytes(p.missing_bytes) }} still needed</span>
                        <span v-if="p.license">{{ p.license }}</span>
                        <a v-if="p.homepage" :href="p.homepage" target="_blank" rel="noopener noreferrer">Model page</a>
                    </div>

                    <div v-if="active(p)" class="dl">
                        <div class="dl-row">
                            <strong>{{ stateText[p.status.state] || p.status.state }}</strong>
                            <span class="spacer" />
                            <span v-if="p.status.state === 'downloading'" class="muted">{{ formatBytes(p.status.speed_bps) }}/s · {{ eta(downloadSeconds(p)) }}</span>
                        </div>
                        <div class="progress" :class="{ 'progress--indeterminate': p.status.state === 'packing' || p.status.state === 'verifying' }">
                            <div :style="{ width: `${ratio(p) * 100}%` }" />
                        </div>
                        <div class="dl-row faint">
                            <span class="mono ellipsis">{{ p.status.detail }}</span>
                            <span class="spacer" />
                            <span>{{ formatBytes(p.status.done_bytes) }} / {{ formatBytes(p.status.total_bytes) }}</span>
                        </div>
                    </div>
                    <p v-if="p.status.state === 'failed'" class="status-message status-message--error">{{ p.status.error }}</p>
                    <p v-if="p.status.state === 'cancelled'" class="status-message">Stopped. Downloaded parts are kept, so it resumes where it left off.</p>

                    <div class="actions">
                        <button v-if="active(p)" class="btn-secondary" type="button" @click="stop(p)"><Icon name="stop" :size="16" />Stop</button>
                        <button v-else-if="!p.installed" class="btn" type="button" :disabled="busy[p.id]" @click="start(p)">
                            <Icon name="download" :size="16" />{{ p.status.state === "failed" || p.status.state === "cancelled" ? "Resume" : "Download" }} {{ formatBytes(p.missing_bytes) }}
                        </button>
                        <button class="btn-ghost" type="button" @click="expanded[p.id] = !expanded[p.id]">{{ expanded[p.id] ? "Hide files" : "Files" }}</button>
                    </div>

                    <ul v-if="expanded[p.id]" class="files">
                        <li v-for="c in p.components" :key="c.path">
                            <Icon :name="c.installed ? 'check' : 'download'" :size="14" :class="c.installed ? 'ok' : 'faint'" />
                            <span class="mono ellipsis">{{ c.path }}</span>
                            <span class="faint">{{ c.packed_from ? "packed · " : "" }}{{ formatBytes(c.size) }}</span>
                        </li>
                    </ul>
                </article>
            </div>
        </section>

        <p v-if="state.downloads && !state.downloads.packages.length" class="empty">No downloadable packages are configured.</p>
    </div>
</template>

<style scoped>
.models-page {
    display: grid;
    gap: 22px;
    padding: 22px;
    max-width: 1200px;
}

.head {
    display: flex;
    align-items: center;
    flex-wrap: wrap;
    gap: 12px;
}

.head p {
    margin: 4px 0 0;
}

.spacer {
    flex: 1;
}

.group {
    display: grid;
    gap: 12px;
}

.group h2 {
    color: var(--text-2);
    font-size: 12px;
    text-transform: uppercase;
    letter-spacing: 0.08em;
}

.cards {
    display: grid;
    grid-template-columns: repeat(auto-fill, minmax(340px, 1fr));
    gap: 14px;
}

.card {
    display: flex;
    flex-direction: column;
    gap: 12px;
    padding: 16px;
}

.card.active {
    border-color: var(--accent);
}

.card-head {
    display: flex;
    align-items: center;
    justify-content: space-between;
    gap: 8px;
}

.desc {
    margin: 0;
    color: var(--text-2);
}

.facts {
    display: flex;
    flex-wrap: wrap;
    gap: 4px 14px;
    color: var(--text-3);
    font-size: 12px;
}

.dl {
    display: grid;
    gap: 7px;
}

.dl-row {
    display: flex;
    align-items: center;
    gap: 10px;
    font-size: 12px;
}

.ellipsis {
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
}

.actions {
    display: flex;
    gap: 8px;
    margin-top: auto;
}

.files {
    display: grid;
    gap: 6px;
    margin: 0;
    padding: 10px 0 0;
    border-top: 1px solid var(--line);
    list-style: none;
}

.files li {
    display: grid;
    grid-template-columns: auto minmax(0, 1fr) auto;
    align-items: center;
    gap: 8px;
    font-size: 12px;
}

.ok {
    color: var(--ok);
}
</style>
