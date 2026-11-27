<script setup lang="ts">
import { computed, onMounted, ref } from "vue";

import AddModelDialog from "../components/AddModelDialog.vue";
import Icon from "../components/Icon.vue";
import InstalledFiles from "../components/InstalledFiles.vue";
import PackageEditor from "../components/PackageEditor.vue";
import PresetsPanel from "../components/PresetsPanel.vue";
import PresetEditor from "../components/PresetEditor.vue";
import { api } from "../lib/api";
import { confirmDanger } from "../lib/confirm";
import { eta, formatBytes, loadCatalog, loadDownloads, startPolling, state } from "../lib/store";
import type { DownloadPackage } from "../lib/types";

type Tab = "download" | "installed" | "presets";
const tab = ref<Tab>("download");
const expanded = ref<Record<string, boolean>>({});
const busy = ref<Record<string, boolean>>({});
const adding = ref(false);
const editingPackage = ref<{ id?: string } | null>(null);
const guideFor = ref("");
const refreshing = ref(false);
const actionError = ref("");

const catalogText = computed(() => {
    const c = state.downloads?.catalog;
    if (!c) return "";
    if (c.state === "ok") return `Online catalog updated ${c.checked ? new Date(c.checked).toLocaleTimeString(undefined, { timeStyle: "short" }) : ""}`;
    if (c.state === "disabled") return "Online catalog off (built-in list)";
    if (c.state === "checking") return "Checking the online catalog…";
    return `Online catalog unavailable: ${c.error || "error"}. Using the ${hasOnlineCopy.value ? "last downloaded" : "built-in"} list.`;
});
const hasOnlineCopy = computed(() => (state.downloads?.packages || []).some((p) => p.source === "online" || p.overrides === "online"));

async function refreshCatalog() {
    refreshing.value = true;
    try {
        await api.refreshCatalog();
        await loadDownloads();
    } finally {
        refreshing.value = false;
    }
}

const ownFile = (p: DownloadPackage) => p.source === `${p.id}.json`;

async function removePackage(p: DownloadPackage) {
    const ok = p.overrides
        ? await confirmDanger("Undo your edits?", `Your copy of "${p.name}" will be deleted and the ${p.overrides} version comes back. Downloaded files stay.`, "Undo edits")
        : await confirmDanger("Remove from the list?", `"${p.name}" will be removed from the Download list. Downloaded files stay; delete them first with Delete files if you want the space back.`, "Remove");
    if (!ok) return;
    actionError.value = "";
    try {
        await api.removeDownload(p.id);
        await loadDownloads();
    } catch (e) {
        actionError.value = (e as Error).message;
    }
}

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

// Other packages and configs that use the same files, so deleting does not surprise anyone.
function sharedUse(p: DownloadPackage, path: string) {
    const users: string[] = [];
    for (const other of state.downloads?.packages || [])
        if (other.id !== p.id && other.components.some((c) => c.path === path)) users.push(`package ${other.name}`);
    for (const recipe of state.recipes)
        if (Object.values(recipe.models || {}).includes(path)) users.push(`config ${recipe.name}`);
    return users;
}

async function deleteFiles(p: DownloadPackage) {
    const files = p.components.filter((c) => c.installed);
    const total = files.reduce((sum, c) => sum + c.size, 0);
    const shared = files.flatMap((c) => sharedUse(p, c.path).map((u) => `${c.path.split("/").pop()} is also used by ${u}`));
    const ok = await confirmDanger(
        `Delete the files of ${p.name}?`,
        `${files.length} file(s), ${formatBytes(total)}, will be removed from disk. This cannot be undone; you can download them again.`,
        "Delete files",
        files.map((c) => `${c.path}  (${formatBytes(c.size)})`),
        shared.length ? shared.join(". ") + "." : undefined,
    );
    if (!ok) return;
    actionError.value = "";
    try {
        await api.deletePackageFiles(p.id);
        await Promise.all([loadDownloads(), loadCatalog()]);
    } catch (e) {
        actionError.value = (e as Error).message;
    }
}

function sourceLabel(p: DownloadPackage) {
    if (p.overrides) return `edited (${p.overrides} original)`;
    if (p.source === "builtin") return "built-in";
    if (p.source === "online") return "online catalog";
    return "added by you";
}
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

        <div class="segmented tabs" role="tablist" aria-label="Model manager sections">
            <button type="button" role="tab" :aria-pressed="tab === 'download'" @click="tab = 'download'"><Icon name="download" :size="16" />Download</button>
            <button type="button" role="tab" :aria-pressed="tab === 'installed'" @click="tab = 'installed'"><Icon name="models" :size="16" />Installed files</button>
            <button type="button" role="tab" :aria-pressed="tab === 'presets'" @click="tab = 'presets'"><Icon name="create" :size="16" />Model configs</button>
        </div>

        <InstalledFiles v-if="tab === 'installed'" />
        <PresetsPanel v-else-if="tab === 'presets'" />
        <template v-else>
        <div class="catalog-bar panel">
            <span class="faint small">{{ catalogText }}</span>
            <span class="spacer" />
            <button class="btn-ghost" type="button" :disabled="refreshing" @click="refreshCatalog"><Icon name="refresh" :size="16" />{{ refreshing ? "Refreshing…" : "Refresh catalog" }}</button>
            <button class="btn-secondary" type="button" @click="editingPackage = {}"><Icon name="create" :size="16" />New package</button>
            <button class="btn" type="button" @click="adding = true"><Icon name="download" :size="16" />Add model</button>
        </div>

        <p v-for="e in state.downloads?.errors || []" :key="e.file + e.message" class="status-message status-message--error">{{ e.file }}: {{ e.message }}</p>
        <p v-if="actionError" class="status-message status-message--error">{{ actionError }}</p>

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
                        <button v-if="p.guide?.inputs" class="btn-ghost" type="button" @click="guideFor = p.id">Create config</button>
                        <span class="spacer" />
                        <button class="btn-ghost icon-btn" type="button" title="Edit this package" aria-label="Edit this package" :disabled="active(p)" @click="editingPackage = { id: p.id }"><Icon name="edit" :size="16" /></button>
                        <button
                            v-if="p.components.some((c) => c.installed)"
                            class="btn-ghost icon-btn btn-danger"
                            type="button"
                            title="Delete the downloaded files"
                            aria-label="Delete the downloaded files"
                            :disabled="active(p)"
                            @click="deleteFiles(p)"
                        >
                            <Icon name="trash" :size="16" />
                        </button>
                        <button v-if="ownFile(p)" class="btn-ghost btn-danger" type="button" :disabled="active(p)" @click="removePackage(p)">
                            {{ p.overrides ? "Reset" : "Remove" }}
                        </button>
                    </div>
                    <p class="faint small">{{ sourceLabel(p) }}<template v-if="ownFile(p) && !p.overrides"> · removing keeps downloaded files</template></p>

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
        </template>

        <AddModelDialog v-if="adding" @close="adding = false" @added="loadDownloads(); startPolling()" />
        <PackageEditor v-if="editingPackage" :package-id="editingPackage.id" @close="editingPackage = null" />
        <PresetEditor v-if="guideFor" mode="new" :guide-package="guideFor" @close="guideFor = ''" />
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

.tabs {
    justify-self: start;
    min-width: min(520px, 100%);
}

.tabs button {
    display: inline-flex;
    align-items: center;
    justify-content: center;
    gap: 7px;
}

.catalog-bar {
    display: flex;
    flex-wrap: wrap;
    align-items: center;
    gap: 8px;
    padding: 10px 12px;
}

.small {
    margin: 0;
    font-size: 12px;
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
