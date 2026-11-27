<script setup lang="ts">
import { computed, onMounted, reactive, ref } from "vue";

import Icon from "./Icon.vue";
import { api } from "../lib/api";
import { categoryLabel, categoryOrder } from "../lib/categories";
import { formatBytes, loadDownloads } from "../lib/store";
import type { PackageDefinition } from "../lib/types";

// Without `packageId` the editor creates a new package.
const props = defineProps<{ packageId?: string }>();
const emit = defineEmits<{ (e: "close"): void }>();

interface FileRow {
    category: string;
    name: string;
    url: string;
    size: number | null;
    sha256: string;
    note: string;
    busy: boolean;
}

const loading = ref(Boolean(props.packageId));
const definition = ref<PackageDefinition | null>(null);
const error = ref("");
const saving = ref(false);
const form = reactive({ id: "", name: "", group: "My models", description: "", license: "", homepage: "", idTouched: false });
const files = ref<FileRow[]>([]);

const isNew = computed(() => !props.packageId);
const extras = computed(() => {
    const pkg = definition.value?.package;
    return { convert: (pkg?.convert || []).length, recipes: (pkg?.recipes || []).length };
});

function emptyRow(): FileRow {
    return { category: "diffusion_models", name: "", url: "", size: null, sha256: "", note: "", busy: false };
}

function slugify(text: string) {
    return text.toLowerCase().replace(/[^a-z0-9]+/g, "-").replace(/^-+|-+$/g, "").slice(0, 100);
}

function onName() {
    if (isNew.value && !form.idTouched) form.id = slugify(form.name);
}

async function lookup(row: FileRow) {
    row.note = "";
    if (!row.url.trim()) {
        row.note = "Paste a link first.";
        return;
    }
    row.busy = true;
    try {
        const info = await api.addCustomModel({ url: row.url.trim(), category: row.category, filename: row.name.trim() || undefined, dry_run: true });
        row.url = info.url;
        row.name = info.path.split("/").slice(1).join("/");
        row.size = info.size || null;
        row.sha256 = info.sha256;
        row.note = info.sha256 ? "Size and SHA-256 from Hugging Face" : info.size ? "Size from the server; no checksum" : "No size or checksum available";
    } catch (e) {
        row.note = (e as Error).message;
    } finally {
        row.busy = false;
    }
}

async function save() {
    error.value = "";
    if (!form.name.trim()) return void (error.value = "Give the package a name.");
    if (!/^[A-Za-z0-9_.-]{1,128}$/.test(form.id)) return void (error.value = "The ID may use letters, digits, dots, dashes and underscores.");
    const rows = files.value.filter((r) => r.url.trim() || r.name.trim());
    if (!rows.length && !extras.value.convert) return void (error.value = "Add at least one file.");
    for (const r of rows)
        if (!r.url.trim() || !r.name.trim()) return void (error.value = "Every file needs a link and a file name.");

    const pkg: Record<string, any> = { ...(definition.value?.package || {}), id: form.id, name: form.name.trim() };
    for (const [key, value] of [["group", form.group], ["description", form.description], ["license", form.license], ["homepage", form.homepage]] as const) {
        if (value.trim()) pkg[key] = value.trim();
        else delete pkg[key];
    }
    pkg.files = rows.map((r) => {
        const file: Record<string, unknown> = { path: `${r.category}/${r.name.trim()}`, url: r.url.trim() };
        if (r.size) file.size = Number(r.size);
        if (r.sha256.trim()) file.sha256 = r.sha256.trim().toLowerCase();
        return file;
    });
    if (!pkg.files.length) delete pkg.files;

    saving.value = true;
    try {
        await api.savePackage(pkg);
        await loadDownloads();
        emit("close");
    } catch (e) {
        error.value = (e as Error).message;
    } finally {
        saving.value = false;
    }
}

onMounted(async () => {
    if (!props.packageId) {
        files.value = [emptyRow()];
        return;
    }
    try {
        definition.value = await api.packageDefinition(props.packageId);
        const pkg = definition.value.package;
        Object.assign(form, {
            id: pkg.id,
            name: pkg.name,
            group: pkg.group || "",
            description: pkg.description || "",
            license: pkg.license || "",
            homepage: pkg.homepage || "",
        });
        files.value = (pkg.files || []).map((f) => {
            const [category, ...rest] = f.path.split("/");
            return { category, name: rest.join("/"), url: f.url, size: f.size ?? null, sha256: f.sha256 || "", note: "", busy: false };
        });
        if (!definition.value.editable) error.value = `This package comes from ${definition.value.source}, which holds several packages; edit that file instead.`;
    } catch (e) {
        error.value = (e as Error).message;
    } finally {
        loading.value = false;
    }
});
</script>

<template>
    <div class="overlay" role="dialog" aria-modal="true" aria-labelledby="package-title" @click.self="emit('close')">
        <form class="dialog panel" @submit.prevent="save">
            <header class="head">
                <div>
                    <h2 id="package-title">{{ isNew ? "New download package" : "Edit download package" }}</h2>
                    <p v-if="definition?.overrides || definition?.source === 'builtin' || definition?.source === 'online'" class="faint small">
                        Saving keeps your own copy in models/downloads/{{ form.id }}.json; <strong>Reset</strong> on the card brings the original back.
                    </p>
                </div>
                <button class="btn-ghost icon-btn" type="button" aria-label="Close" @click="emit('close')"><Icon name="close" /></button>
            </header>

            <p v-if="loading" class="loading-row"><span class="spin" />Loading the package…</p>
            <div v-else class="body">
                <div class="row2">
                    <label class="field"><span>Name</span><input v-model="form.name" class="input" @input="onName" /></label>
                    <label class="field"><span>ID</span><input v-model="form.id" class="input mono" :disabled="!isNew" @input="form.idTouched = true" /></label>
                </div>
                <label class="field"><span>Description</span><textarea v-model="form.description" class="input" rows="2" /></label>
                <div class="row3">
                    <label class="field"><span>Group</span><input v-model="form.group" class="input" placeholder="e.g. Wan 2.2" /></label>
                    <label class="field"><span>License</span><input v-model="form.license" class="input" placeholder="e.g. Apache-2.0" /></label>
                    <label class="field"><span>Model page</span><input v-model="form.homepage" class="input mono" placeholder="https://…" /></label>
                </div>

                <h3>Files</h3>
                <div v-for="(row, index) in files" :key="index" class="file panel">
                    <div class="file-top">
                        <select v-model="row.category" class="input cat" aria-label="Type">
                            <option v-for="c in categoryOrder" :key="c" :value="c">{{ categoryLabel(c) }}</option>
                        </select>
                        <input v-model="row.name" class="input mono" placeholder="file-name.gguf" aria-label="File name" />
                        <button class="btn-ghost icon-btn btn-danger" type="button" aria-label="Remove this file" @click="files.splice(index, 1)"><Icon name="trash" :size="16" /></button>
                    </div>
                    <div class="file-url">
                        <input v-model="row.url" class="input mono" placeholder="https://huggingface.co/owner/repo/blob/main/file.gguf" aria-label="Link" />
                        <button class="btn-secondary" type="button" :disabled="row.busy" @click="lookup(row)"><span v-if="row.busy" class="spin" />{{ row.busy ? "Checking…" : "Look up" }}</button>
                    </div>
                    <div class="file-meta">
                        <label class="mini">Size (bytes) <input v-model.number="row.size" class="input mono" type="number" min="0" placeholder="unknown" /></label>
                        <label class="mini grow">SHA-256 <input v-model="row.sha256" class="input mono" placeholder="optional; checked after download" /></label>
                    </div>
                    <p v-if="row.note || row.size" class="faint small">{{ row.size ? formatBytes(row.size) : "" }}{{ row.note ? ` · ${row.note}` : "" }}</p>
                </div>
                <button class="btn-ghost add" type="button" @click="files.push(emptyRow())"><Icon name="download" :size="16" />Add a file</button>

                <p v-if="extras.convert || extras.recipes" class="faint small">
                    Kept unchanged: {{ extras.convert }} GGUF packing step(s) and {{ extras.recipes }} preset(s) installed with this package.
                </p>
            </div>

            <p v-if="error" class="status-message status-message--error">{{ error }}</p>
            <footer class="actions">
                <button class="btn-ghost" type="button" @click="emit('close')">Cancel</button>
                <button class="btn" type="submit" :disabled="saving || loading || (definition !== null && !definition.editable)">
                    <span v-if="saving" class="spin" /><Icon v-else name="check" :size="16" />{{ saving ? "Saving…" : "Save package" }}
                </button>
            </footer>
        </form>
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
    width: min(800px, 100%);
    max-height: calc(100dvh - 40px);
    padding: 20px;
}

.head {
    display: flex;
    align-items: flex-start;
    justify-content: space-between;
}

.head p {
    margin: 2px 0 0;
}

.body {
    display: grid;
    gap: 14px;
    overflow-y: auto;
    padding-right: 4px;
}

h3 {
    margin-top: 6px;
    color: var(--text-2);
    font-size: 12px;
    text-transform: uppercase;
    letter-spacing: 0.08em;
}

.row2 {
    display: grid;
    grid-template-columns: 1fr 1fr;
    gap: 12px;
}

.row3 {
    display: grid;
    grid-template-columns: 1fr 1fr 1.4fr;
    gap: 12px;
}

.file {
    display: grid;
    gap: 8px;
    padding: 12px;
}

.file-top {
    display: grid;
    grid-template-columns: 190px minmax(0, 1fr) auto;
    gap: 8px;
}

.file-url {
    display: grid;
    grid-template-columns: minmax(0, 1fr) auto;
    gap: 8px;
}

.file-meta {
    display: grid;
    grid-template-columns: 180px minmax(0, 1fr);
    gap: 8px;
}

.mini {
    display: grid;
    gap: 4px;
    color: var(--text-3);
    font-size: 11px;
}

.small {
    margin: 0;
    font-size: 12px;
}

.add {
    justify-self: start;
}

.actions {
    display: flex;
    justify-content: flex-end;
    gap: 8px;
}

@media (max-width: 700px) {
    .row2,
    .row3,
    .file-top,
    .file-meta {
        grid-template-columns: 1fr;
    }
}
</style>
