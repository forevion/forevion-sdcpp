<script setup lang="ts">
import { onBeforeUnmount, onMounted, ref } from "vue";

import Icon from "./Icon.vue";
import { api } from "../lib/api";
import type { FolderListing } from "../lib/types";

const props = defineProps<{ title: string; start: string }>();
const emit = defineEmits<{ (e: "pick", path: string): void; (e: "close"): void }>();

const listing = ref<FolderListing | null>(null);
const typed = ref("");
const loading = ref(false);
const error = ref("");
const newName = ref("");
const creating = ref(false);

async function open(path: string) {
    loading.value = true;
    error.value = "";
    try {
        listing.value = await api.listFolder(path);
        typed.value = listing.value.path;
    } catch (e) {
        error.value = (e as Error).message;
    } finally {
        loading.value = false;
    }
}

async function createFolder() {
    const name = newName.value.trim();
    if (!name || !listing.value) return;
    if (/[\\/]/.test(name)) {
        error.value = "A folder name cannot contain / or \\.";
        return;
    }
    creating.value = true;
    try {
        const separator = listing.value.path.includes("\\") ? "\\" : "/";
        const made = await api.makeFolder(listing.value.path.replace(/[\\/]+$/, "") + separator + name);
        newName.value = "";
        await open(made.path);
    } catch (e) {
        error.value = (e as Error).message;
    } finally {
        creating.value = false;
    }
}

function onKey(event: KeyboardEvent) {
    if (event.key === "Escape") emit("close");
}

// Start in the folder's parent when the folder itself does not exist yet.
onMounted(async () => {
    window.addEventListener("keydown", onKey);
    await open(props.start);
    if (error.value && props.start) {
        const parent = props.start.replace(/[\\/]+[^\\/]*[\\/]*$/, "");
        await open(parent || "");
    }
});
onBeforeUnmount(() => window.removeEventListener("keydown", onKey));
</script>

<template>
    <div class="overlay" role="dialog" aria-modal="true" aria-labelledby="picker-title" @click.self="emit('close')">
        <div class="picker panel">
            <header class="picker-head">
                <h2 id="picker-title">{{ title }}</h2>
                <button class="icon-btn" type="button" aria-label="Close" @click="emit('close')"><Icon name="close" :size="18" /></button>
            </header>

            <form class="path-row" @submit.prevent="open(typed)">
                <button class="btn-secondary" type="button" title="Up one folder" :disabled="!listing?.parent || loading" @click="open(listing!.parent)">
                    <Icon name="chevron" :size="14" class="up" />Up
                </button>
                <input v-model="typed" class="input mono" spellcheck="false" aria-label="Folder path" />
                <button class="btn-secondary" type="submit" :disabled="loading">Go</button>
            </form>

            <div class="body">
                <nav class="places" aria-label="Places">
                    <button v-for="root in listing?.roots || []" :key="root.path" type="button" class="place" :title="root.path" @click="open(root.path)">
                        {{ root.name }}
                    </button>
                </nav>
                <div class="list" :aria-busy="loading">
                    <p v-if="loading" class="loading-row"><span class="spin" />Loading…</p>
                    <template v-else-if="listing">
                        <button v-for="dir in listing.dirs" :key="dir.path" type="button" class="dir" @dblclick="open(dir.path)" @click="open(dir.path)">
                            <span class="folder-icon" aria-hidden="true">▸</span>{{ dir.name }}
                        </button>
                        <p v-if="!listing.dirs.length" class="faint empty">No folders here.</p>
                    </template>
                </div>
            </div>

            <p v-if="error" class="status-message status-message--error">{{ error }}</p>
            <p v-else-if="listing?.warning" class="faint small">Note: {{ listing.warning }}</p>

            <form class="new-row" @submit.prevent="createFolder">
                <input v-model="newName" class="input" placeholder="New folder name" aria-label="New folder name" />
                <button class="btn-secondary" type="submit" :disabled="!newName.trim() || creating || !listing">
                    <span v-if="creating" class="spin" />Create folder
                </button>
            </form>

            <footer class="actions">
                <span class="chosen mono" :title="listing?.path">{{ listing?.path }}</span>
                <button class="btn-ghost" type="button" @click="emit('close')">Cancel</button>
                <button class="btn" type="button" :disabled="!listing || loading" @click="emit('pick', listing!.path)">Use this folder</button>
            </footer>
        </div>
    </div>
</template>

<style scoped>
.overlay {
    position: fixed;
    inset: 0;
    z-index: 90;
    display: grid;
    place-items: center;
    padding: 16px;
    background: rgba(5, 5, 8, 0.78);
}

.picker {
    display: grid;
    gap: 12px;
    width: min(720px, 100%);
    max-height: calc(100dvh - 32px);
    padding: 18px;
    overflow: hidden;
    grid-template-columns: minmax(0, 1fr);
    grid-template-rows: auto auto minmax(0, 1fr) auto auto auto;
}

.picker > * {
    min-width: 0;
}

.picker-head {
    display: flex;
    align-items: center;
    justify-content: space-between;
}

.picker-head h2 {
    margin: 0;
    font-size: 17px;
}

.path-row,
.new-row {
    display: flex;
    gap: 8px;
}

.path-row .input,
.new-row .input {
    flex: 1;
    min-width: 0;
}

.up {
    transform: rotate(-90deg);
}

.body {
    display: grid;
    grid-template-columns: 150px minmax(0, 1fr);
    gap: 10px;
    min-height: 220px;
    overflow: hidden;
}

.places,
.list {
    display: flex;
    flex-direction: column;
    gap: 2px;
    overflow-y: auto;
    padding: 6px;
    border-radius: var(--radius-sm);
    background: var(--bg-sunken);
}

.place,
.dir {
    padding: 7px 10px;
    border: 0;
    border-radius: 8px;
    background: transparent;
    color: var(--text);
    text-align: left;
    cursor: pointer;
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
}

.place {
    color: var(--text-2);
    font-weight: 600;
}

.place:hover,
.dir:hover {
    background: var(--bg-hover);
}

.folder-icon {
    margin-right: 8px;
    color: var(--accent-strong);
}

.empty {
    padding: 10px;
}

.small {
    margin: 0;
    font-size: 12px;
}

.actions {
    display: flex;
    align-items: center;
    gap: 8px;
}

.chosen {
    flex: 1;
    min-width: 0;
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
    color: var(--text-2);
    font-size: 12px;
}

@media (max-width: 560px) {
    .body {
        grid-template-columns: 1fr;
        grid-template-rows: auto minmax(160px, 1fr);
    }

    .places {
        flex-direction: row;
        flex-wrap: wrap;
    }

    .actions {
        flex-wrap: wrap;
    }

    .chosen {
        flex-basis: 100%;
    }
}
</style>
