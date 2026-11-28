<script setup lang="ts">
import { computed, onMounted, ref } from "vue";

import AddModelDialog from "./AddModelDialog.vue";
import FolderSetup from "./FolderSetup.vue";
import Icon from "./Icon.vue";
import InstalledFiles from "./InstalledFiles.vue";
import PackageEditor from "./PackageEditor.vue";
import { api } from "../lib/api";
import { ask } from "../lib/confirm";
import { loadCatalog, loadDownloads, startPolling, state } from "../lib/store";

const emit = defineEmits<{ (e: "close"): void }>();

type Section = "folders" | "storage" | "backup" | "catalog";
const section = ref<Section>("folders");
const busy = ref("");
const message = ref("");
const error = ref("");
const fileInput = ref<HTMLInputElement | null>(null);
const addingFile = ref(false);
const newPackage = ref(false);

const catalogText = computed(() => {
    const c = state.downloads?.catalog;
    if (!c) return "";
    if (c.state === "ok") return `Online list updated at ${c.checked ? new Date(c.checked).toLocaleTimeString(undefined, { timeStyle: "short" }) : "?"}.`;
    if (c.state === "disabled") return "The online list is turned off (server started with --catalog-url none); only built-in and your own entries are shown.";
    if (c.state === "checking") return "Checking the online list…";
    return `The online list is unavailable (${c.error || "error"}), so the built-in or last downloaded list is used.`;
});

async function work(key: string, action: () => Promise<void>) {
    busy.value = key;
    error.value = "";
    message.value = "";
    try {
        await action();
    } catch (e) {
        error.value = (e as Error).message;
    } finally {
        busy.value = "";
    }
}

// Where the online model list comes from. "develop" has catalog edits before they are released.
const CATALOG_BASE = "https://raw.githubusercontent.com/forevion/forevion-sdcpp";
const catalogChoices = [
    { id: "master", label: "Released (master)", url: `${CATALOG_BASE}/master/examples/server/downloads.json` },
    { id: "develop", label: "Latest (develop)", url: `${CATALOG_BASE}/develop/examples/server/downloads.json` },
    { id: "custom", label: "Other address", url: "" },
    { id: "none", label: "Off (built-in list only)", url: "" },
];
const catalogUrl = ref("");
const catalogChoice = ref("master");
const customUrl = ref("");
const canSaveCatalog = ref(false);

async function loadCatalogSource() {
    try {
        const source = await api.catalogSource();
        catalogUrl.value = source.url;
        canSaveCatalog.value = source.can_save;
        const known = catalogChoices.find((c) => c.url && c.url === source.url);
        catalogChoice.value = known ? known.id : source.url ? "custom" : "none";
        customUrl.value = known ? "" : source.url;
    } catch {
        canSaveCatalog.value = false;
    }
}

const chosenCatalogUrl = computed(() =>
    catalogChoice.value === "custom" ? customUrl.value.trim() : catalogChoices.find((c) => c.id === catalogChoice.value)?.url || "",
);

const saveCatalogSource = () => work("source", async () => {
    await api.saveCatalogSource(chosenCatalogUrl.value);
    catalogUrl.value = chosenCatalogUrl.value;
    if (catalogUrl.value) await api.refreshCatalog();
    await loadDownloads();
    message.value = catalogUrl.value ? "Model list source saved and refreshed." : "Online model list turned off.";
});

onMounted(loadCatalogSource);

const refreshCatalog = () => work("catalog", async () => {
    await api.refreshCatalog();
    await loadDownloads();
    message.value = "Model list refreshed.";
});

const rescan = () => work("rescan", async () => {
    await loadCatalog(true);
    await loadDownloads();
    message.value = "Model folders rescanned.";
});

const backup = () => work("backup", async () => {
    const response = await fetch("/sdcpp/v1/config/backup", { cache: "no-store" });
    if (!response.ok) throw new Error(`backup failed (HTTP ${response.status})`);
    const blob = await response.blob();
    const stamp = new Date().toISOString().slice(0, 16).replace(/[-:T]/g, "");
    const link = Object.assign(document.createElement("a"), { href: URL.createObjectURL(blob), download: `sdcpp-config-${stamp}.json` });
    link.click();
    URL.revokeObjectURL(link.href);
    message.value = "Backup saved to your downloads folder.";
});

async function restore(event: Event) {
    const input = event.target as HTMLInputElement;
    const file = input.files?.[0];
    input.value = "";
    if (!file) return;
    error.value = "";
    message.value = "";
    let data: any;
    try {
        data = JSON.parse(await file.text());
    } catch {
        error.value = `${file.name} is not a JSON file.`;
        return;
    }
    if (data?.kind !== "sdcpp-studio-config") {
        error.value = `${file.name} is not a Forevion Studio backup.`;
        return;
    }
    const variants: string[] = (data.presets || []).map((p: any) => `variant  ${p.file}`);
    const models: string[] = (data.packages || []).map((p: any) => `model    ${p.file}`);
    const mode = await ask({
        title: "Restore from this backup?",
        message: `${file.name} from ${data.created ? new Date(data.created).toLocaleString() : "an unknown date"} has ${variants.length} variant(s) and ${models.length} model entr${models.length === 1 ? "y" : "ies"}.`,
        details: [...variants, ...models],
        warning: "Merge keeps everything else and overwrites entries with the same file name. Replace first deletes all your variants and added models. Model files on disk are never touched.",
        choices: [
            { label: "Replace all", value: "replace", danger: true },
            { label: "Merge", value: "merge", primary: true },
        ],
    });
    if (!mode) return;
    await work("restore", async () => {
        const response = await fetch("/sdcpp/v1/config/restore", {
            method: "POST",
            headers: { "Content-Type": "application/json" },
            body: JSON.stringify({ backup: data, mode }),
        });
        const result = await response.json();
        if (!response.ok) {
            const details = (result.errors || []).map((e: any) => `${e.file}: ${e.message}`).join("; ");
            throw new Error(`${result.error || "restore failed"}${details ? ` — ${details}` : ""}`);
        }
        await Promise.all([loadCatalog(), loadDownloads()]);
        message.value = `Restored ${result.written} file(s)${mode === "replace" ? `, removed ${result.removed} old one(s)` : ""}.`;
    });
}
</script>

<template>
    <div class="overlay" role="dialog" aria-modal="true" aria-labelledby="settings-title" @click.self="emit('close')">
        <div class="dialog panel">
            <header class="head">
                <h2 id="settings-title">Model settings</h2>
                <button class="btn-ghost icon-btn" type="button" aria-label="Close" @click="emit('close')"><Icon name="close" /></button>
            </header>
            <nav class="segmented tabs" role="tablist">
                <button type="button" role="tab" :aria-pressed="section === 'folders'" @click="section = 'folders'">Folders</button>
                <button type="button" role="tab" :aria-pressed="section === 'storage'" @click="section = 'storage'">Storage</button>
                <button type="button" role="tab" :aria-pressed="section === 'backup'" @click="section = 'backup'">Backup and restore</button>
                <button type="button" role="tab" :aria-pressed="section === 'catalog'" @click="section = 'catalog'">Model list</button>
            </nav>
            <p v-if="message" class="status-message">{{ message }}</p>
            <p v-if="error" class="status-message status-message--error">{{ error }}</p>

            <div class="body">
                <FolderSetup v-if="section === 'folders'" />
                <InstalledFiles v-else-if="section === 'storage'" />

                <div v-else-if="section === 'backup'" class="block">
                    <p class="muted">Save all your variants and added or edited models in one file, or load them back on this or another computer. Model files are not included.</p>
                    <div class="row">
                        <button class="btn-secondary" type="button" :disabled="!!busy" @click="backup">
                            <span v-if="busy === 'backup'" class="spin" /><Icon v-else name="download" :size="16" />Download backup
                        </button>
                        <button class="btn-secondary" type="button" :disabled="!!busy" @click="fileInput?.click()">
                            <span v-if="busy === 'restore'" class="spin" /><Icon v-else name="reuse" :size="16" />{{ busy === "restore" ? "Restoring…" : "Restore from file…" }}
                        </button>
                        <input ref="fileInput" class="sr-only" type="file" accept=".json,application/json" @change="restore" />
                    </div>
                </div>

                <div v-else class="block">
                    <p class="muted">{{ catalogText }}</p>
                    <div class="source">
                        <label class="field">
                            <span>Where the model list comes from</span>
                            <select v-model="catalogChoice" class="input" :disabled="!canSaveCatalog">
                                <option v-for="choice in catalogChoices" :key="choice.id" :value="choice.id">{{ choice.label }}</option>
                            </select>
                        </label>
                        <input v-if="catalogChoice === 'custom'" v-model="customUrl" class="input mono" placeholder="https://…/downloads.json" :disabled="!canSaveCatalog" />
                        <button
                            v-if="canSaveCatalog"
                            class="btn-secondary"
                            type="button"
                            :disabled="!!busy || chosenCatalogUrl === catalogUrl || (catalogChoice === 'custom' && !customUrl.trim())"
                            @click="saveCatalogSource"
                        >
                            <span v-if="busy === 'source'" class="spin" /><Icon v-else name="check" :size="16" />Use this list
                        </button>
                    </div>
                    <p class="faint small">
                        Model entries are plain data: an edit to <code>downloads.json</code> on GitHub shows up here after <strong>Refresh model list</strong>, with no rebuild or
                        restart. Pick <strong>Latest (develop)</strong> to get edits before they are released.
                    </p>
                    <div class="row">
                        <button class="btn-secondary" type="button" :disabled="!!busy" @click="refreshCatalog">
                            <span v-if="busy === 'catalog'" class="spin" /><Icon v-else name="refresh" :size="16" />Refresh model list
                        </button>
                        <button class="btn-secondary" type="button" :disabled="!!busy" @click="rescan">
                            <span v-if="busy === 'rescan'" class="spin" /><Icon v-else name="refresh" :size="16" />Rescan model folders
                        </button>
                    </div>
                    <p class="faint small">Rescan picks up files you copied into {{ state.downloads?.model_root }} by hand.</p>
                    <h3>Expert</h3>
                    <div class="row">
                        <button class="btn-ghost" type="button" @click="addingFile = true"><Icon name="download" :size="16" />Download a single file</button>
                        <button class="btn-ghost" type="button" @click="newPackage = true"><Icon name="create" :size="16" />New download entry</button>
                    </div>
                    <p class="faint small">To set up a new model step by step, use <strong>Add your own model</strong> on the Models page instead.</p>
                </div>
            </div>
        </div>
        <AddModelDialog v-if="addingFile" @close="addingFile = false" @added="loadDownloads(); startPolling()" />
        <PackageEditor v-if="newPackage" @close="newPackage = false" />
    </div>
</template>

<style scoped>
.source {
    display: grid;
    gap: 8px;
}

.overlay {
    position: fixed;
    inset: 0;
    z-index: 45;
    display: grid;
    place-items: center;
    padding: 20px;
    background: rgba(5, 5, 8, 0.75);
}

.dialog {
    display: grid;
    grid-template-rows: auto auto auto auto minmax(0, 1fr);
    gap: 12px;
    width: min(980px, 100%);
    height: min(780px, 100%);
    padding: 20px;
    max-height: calc(100dvh - 40px);
}

.head {
    display: flex;
    align-items: center;
    justify-content: space-between;
}

.tabs {
    justify-self: start;
}

.body {
    overflow-y: auto;
    padding-right: 4px;
}

.block {
    display: grid;
    gap: 12px;
}

.block p {
    margin: 0;
}

.row {
    display: flex;
    flex-wrap: wrap;
    gap: 8px;
}

h3 {
    margin-top: 10px;
    color: var(--text-2);
    font-size: 12px;
    text-transform: uppercase;
    letter-spacing: 0.08em;
}

.small {
    font-size: 12px;
}
</style>
