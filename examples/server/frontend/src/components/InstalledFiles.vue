<script setup lang="ts">
import { computed, onMounted, ref } from "vue";

import Icon from "./Icon.vue";
import { api } from "../lib/api";
import { confirmDanger } from "../lib/confirm";
import { categoryLabel } from "../lib/categories";
import { formatBytes, loadCatalog, loadDownloads } from "../lib/store";
import type { StorageInfo } from "../lib/types";

const storage = ref<StorageInfo | null>(null);
const error = ref("");
const filter = ref("all");
const search = ref("");
const busy = ref("");

const categories = computed(() => [...new Set((storage.value?.files || []).map((f) => f.category))].sort());
const files = computed(() => {
    const q = search.value.trim().toLowerCase();
    return (storage.value?.files || [])
        .filter((f) => (filter.value === "all" || f.category === filter.value) && (!q || f.id.toLowerCase().includes(q)))
        .sort((a, b) => b.size - a.size);
});
const totalSize = computed(() => files.value.reduce((sum, f) => sum + f.size, 0));

async function load() {
    try {
        storage.value = await api.storage();
        error.value = "";
    } catch (e) {
        error.value = (e as Error).message;
    }
}

async function remove(file: StorageInfo["files"][number]) {
    const ok = await confirmDanger(
        "Delete this model file?",
        `${file.name} (${formatBytes(file.size)}) will be removed from disk. This cannot be undone; you can download it again later.`,
        "Delete file",
        undefined,
        file.used_by.length ? `Used by: ${file.used_by.map((r) => r.name).join(", ")}. Those configs will need another file.` : undefined,
    );
    if (!ok) return;
    const id = file.id;
    busy.value = id;
    try {
        await api.deleteModelFile(id);
        await Promise.all([load(), loadCatalog(), loadDownloads()]);
    } catch (e) {
        error.value = (e as Error).message;
    } finally {
        busy.value = "";
    }
}

function when(ms: number) {
    return ms ? new Date(ms).toLocaleDateString(undefined, { dateStyle: "medium" }) : "";
}

onMounted(load);
defineExpose({ load });
</script>

<template>
    <section class="installed">
        <div v-for="root in storage?.roots || []" :key="root.id" class="disk panel">
            <div class="disk-row">
                <span class="mono ellipsis">{{ root.path }}</span>
                <span class="spacer" />
                <strong>{{ formatBytes(root.free_bytes) }} free</strong>
                <span class="faint">of {{ formatBytes(root.total_bytes) }}</span>
            </div>
            <div class="progress">
                <div :style="{ width: `${root.total_bytes ? (1 - root.free_bytes / root.total_bytes) * 100 : 0}%` }" />
            </div>
        </div>

        <div class="tools">
            <input v-model="search" class="input search" placeholder="Search files" />
            <select v-model="filter" class="input type" aria-label="Filter by type">
                <option value="all">All types</option>
                <option v-for="c in categories" :key="c" :value="c">{{ categoryLabel(c) }}</option>
            </select>
            <span class="spacer" />
            <span class="faint">{{ files.length }} files · {{ formatBytes(totalSize) }}</span>
            <button class="btn-ghost icon-btn" type="button" title="Refresh" aria-label="Refresh" @click="load"><Icon name="refresh" :size="16" /></button>
        </div>

        <p v-if="error" class="status-message status-message--error">{{ error }}</p>

        <div class="table panel">
            <div v-for="file in files" :key="file.id" class="row">
                <div class="main">
                    <span class="mono name">{{ file.name }}</span>
                    <span class="meta">
                        <span class="chip">{{ categoryLabel(file.category) }}</span>
                        <span v-if="file.loaded" class="chip chip--accent">loaded now</span>
                        <span v-for="r in file.used_by" :key="r.id" class="chip chip--ok" :title="`Used by the preset ${r.name}`">{{ r.name }}</span>
                        <span v-if="!file.used_by.length" class="faint small">not used by any preset</span>
                    </span>
                </div>
                <span class="size">{{ formatBytes(file.size) }}</span>
                <span class="date faint">{{ when(file.modified) }}</span>
                <div class="act">
                    <button
                        class="btn-ghost btn-danger"
                        type="button"
                        :disabled="file.loaded || busy === file.id"
                        :title="file.loaded ? 'Part of the loaded model' : 'Delete this file'"
                        @click="remove(file)"
                    >
                        <Icon name="trash" :size="16" />Delete
                    </button>
                </div>
            </div>
            <p v-if="storage && !files.length" class="empty">No model files found.</p>
        </div>
    </section>
</template>

<style scoped>
.installed {
    display: grid;
    gap: 14px;
}

.disk {
    display: grid;
    gap: 8px;
    padding: 12px 14px;
}

.disk-row,
.tools {
    display: flex;
    align-items: center;
    gap: 10px;
    min-width: 0;
}

.spacer {
    flex: 1;
}

.ellipsis {
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
}

.search {
    width: min(260px, 100%);
}

.type {
    width: 200px;
}

.table {
    display: grid;
    overflow: hidden;
}

.row {
    display: grid;
    grid-template-columns: minmax(0, 1fr) 90px 110px auto;
    align-items: center;
    gap: 14px;
    padding: 11px 14px;
    border-bottom: 1px solid var(--line);
}

.row:last-child {
    border-bottom: 0;
}

.main {
    display: grid;
    gap: 6px;
    min-width: 0;
}

.name {
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
    font-size: 13px;
}

.meta {
    display: flex;
    flex-wrap: wrap;
    align-items: center;
    gap: 6px;
}

.small {
    font-size: 12px;
}

.size {
    text-align: right;
    font-weight: 600;
}

.act {
    display: flex;
    justify-content: flex-end;
    gap: 4px;
}

@media (max-width: 760px) {
    .row {
        grid-template-columns: 1fr auto;
    }

    .date {
        display: none;
    }

    .act {
        grid-column: 1 / -1;
        justify-content: flex-start;
    }
}
</style>
