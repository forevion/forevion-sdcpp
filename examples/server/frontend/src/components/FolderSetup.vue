<script setup lang="ts">
import { computed, onMounted, reactive, ref } from "vue";

import FolderPicker from "./FolderPicker.vue";
import Icon from "./Icon.vue";
import { api } from "../lib/api";
import { loadCatalog, loadDownloads, loadOutputs } from "../lib/store";
import type { FolderKey, FolderSettings } from "../lib/types";

const props = defineProps<{ firstRun?: boolean }>();
const emit = defineEmits<{ (e: "saved"): void }>();

const info = ref<FolderSettings | null>(null);
const paths = reactive<Record<FolderKey, string>>({ models: "", inputs: "", outputs: "" });
const loading = ref(true);
const saving = ref(false);
const error = ref("");
const message = ref("");
const browsing = ref<FolderKey | "">("");

const rows: { key: FolderKey; label: string; help: string }[] = [
    {
        key: "models",
        label: "Models",
        help: "Model files: downloads go into diffusion_models/, text_encoders/, vae/ … inside it. Pick the folder that already holds your models to use them.",
    },
    { key: "inputs", label: "Inputs", help: "Start images, reference images, audio and control videos you add on Create are kept here." },
    { key: "outputs", label: "Outputs", help: "Finished images and videos; the Gallery shows this folder." },
];

const changed = computed(() => Boolean(info.value && rows.some((row) => paths[row.key].trim() !== info.value!.folders[row.key].path)));

async function load() {
    loading.value = true;
    error.value = "";
    try {
        info.value = await api.folders();
        for (const row of rows) {
            const current = info.value.folders[row.key];
            // On first start suggest folders next to the app, unless one is already in use.
            paths[row.key] = info.value.needs_setup && !current.exists ? info.value.suggested[row.key] : current.path;
        }
    } catch (e) {
        error.value = (e as Error).message;
    } finally {
        loading.value = false;
    }
}

async function save() {
    saving.value = true;
    error.value = "";
    message.value = "";
    try {
        await api.saveFolders({ models: paths.models.trim(), inputs: paths.inputs.trim(), outputs: paths.outputs.trim() });
        api.refreshCatalog().then(loadDownloads, () => undefined);
        await Promise.all([loadCatalog(true), loadDownloads(), loadOutputs()]);
        await load();
        message.value = "Folders saved and in use.";
        emit("saved");
    } catch (e) {
        error.value = (e as Error).message;
    } finally {
        saving.value = false;
    }
}

function picked(path: string) {
    if (browsing.value) paths[browsing.value] = path;
    browsing.value = "";
}

onMounted(load);
</script>

<template>
    <div class="folder-setup">
        <p v-if="loading" class="loading-row"><span class="spin" />Loading folders…</p>
        <template v-else-if="info">
            <p v-if="props.firstRun" class="intro">
                Choose where Forevion Studio keeps its files. Folders that do not exist yet are created. You can change them later in
                <strong>Models → Settings → Folders</strong>.
            </p>
            <p v-else-if="!info.can_save" class="status-message">
                The server was started without <code>--settings-file</code>, so these folders come from its command line
                (<code>--model-dir</code>, <code>--input-dir</code>, <code>--output-dir</code>) and cannot be changed here.
            </p>

            <div v-for="row in rows" :key="row.key" class="row">
                <label class="field">
                    <span>{{ row.label }}</span>
                    <div class="path">
                        <input v-model="paths[row.key]" class="input mono" spellcheck="false" :readonly="!info.can_save" :placeholder="info.suggested[row.key]" />
                        <button v-if="info.can_save && info.can_browse" class="btn-secondary" type="button" @click="browsing = row.key">Browse…</button>
                    </div>
                    <small class="faint">{{ row.help }}</small>
                </label>
            </div>

            <p v-if="info.can_save && !info.can_browse" class="faint small">Browsing works only on the computer that runs the server; type or paste full paths here.</p>
            <p v-if="error" class="status-message status-message--error">{{ error }}</p>
            <p v-if="message" class="status-message">{{ message }}</p>

            <footer v-if="info.can_save" class="actions">
                <span class="faint small mono settings-file" :title="info.settings_file">Saved in {{ info.settings_file }}</span>
                <button class="btn" type="button" :disabled="saving || (!props.firstRun && !changed)" @click="save">
                    <span v-if="saving" class="spin" /><Icon v-else name="check" :size="16" />{{ props.firstRun ? "Save and continue" : "Save folders" }}
                </button>
            </footer>
        </template>
        <p v-else-if="error" class="status-message status-message--error">{{ error }}</p>

        <FolderPicker
            v-if="browsing"
            :title="`Choose the ${browsing} folder`"
            :start="paths[browsing] || info?.suggested[browsing] || ''"
            @pick="picked"
            @close="browsing = ''"
        />
    </div>
</template>

<style scoped>
.folder-setup {
    display: grid;
    grid-template-columns: minmax(0, 1fr);
    gap: 16px;
}

.folder-setup > * {
    min-width: 0;
}

.intro {
    margin: 0;
    color: var(--text-2);
    line-height: 1.55;
}

.row .field {
    display: grid;
    gap: 6px;
}

.path {
    display: flex;
    gap: 8px;
}

.path .input {
    flex: 1;
    min-width: 0;
}

.small {
    margin: 0;
    font-size: 12px;
}

.actions {
    display: flex;
    align-items: center;
    flex-wrap: wrap;
    gap: 10px;
}

.settings-file {
    flex: 1;
    min-width: 0;
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
}
</style>
