<script setup lang="ts">
import { computed, nextTick, ref, watch } from "vue";

import Icon from "./Icon.vue";
import { api } from "../lib/api";
import { categoryLabel, categoryOrder } from "../lib/categories";
import { formatBytes } from "../lib/store";
import type { CustomModelInfo } from "../lib/types";

const props = defineProps<{ category?: string }>();
const emit = defineEmits<{ (e: "close"): void; (e: "added", id: string): void }>();

const url = ref("");
const category = ref(props.category || "diffusion_models");
const filename = ref("");
const name = ref("");
const info = ref<CustomModelInfo | null>(null);
const error = ref("");
const busy = ref(false);

const isHuggingFace = computed(() => /^https:\/\/huggingface\.co\//.test(url.value.trim()));

watch([url, category, filename], () => {
    info.value = null;
    error.value = "";
});

function request(dryRun: boolean) {
    return {
        url: url.value.trim(),
        category: category.value,
        filename: filename.value.trim() || undefined,
        name: name.value.trim() || undefined,
        dry_run: dryRun,
    };
}

async function check() {
    if (!url.value.trim()) {
        error.value = "Paste a link to the model file first.";
        return;
    }
    busy.value = true;
    error.value = "";
    try {
        const result = await api.addCustomModel(request(true));
        // Filling "Save as" triggers the reset watcher; set the result after it ran.
        if (!filename.value.trim()) {
            filename.value = result.path.split("/").pop() || "";
            await nextTick();
        }
        info.value = result;
    } catch (e) {
        error.value = (e as Error).message;
    } finally {
        busy.value = false;
    }
}

async function add() {
    if (!info.value) return check();
    busy.value = true;
    error.value = "";
    try {
        const result = await api.addCustomModel(request(false));
        emit("added", result.id || "");
        emit("close");
    } catch (e) {
        error.value = (e as Error).message;
    } finally {
        busy.value = false;
    }
}
</script>

<template>
    <div class="overlay" role="dialog" aria-modal="true" aria-labelledby="add-model-title" @click.self="emit('close')">
        <form class="dialog panel" @submit.prevent="add">
            <header class="head">
                <h2 id="add-model-title">{{ props.category === "loras" ? "Add a LoRA" : "Add a model" }}</h2>
                <button class="btn-ghost icon-btn" type="button" aria-label="Close" @click="emit('close')"><Icon name="close" /></button>
            </header>

            <label class="field">
                <span>Link to the file</span>
                <input v-model="url" class="input mono" placeholder="https://huggingface.co/owner/repo/blob/main/model.safetensors" autofocus />
                <small class="faint">
                    A direct link to one .gguf / .safetensors file. On Hugging Face, open the file and copy the page address.
                </small>
            </label>

            <div class="row">
                <label class="field">
                    <span>Type</span>
                    <select v-model="category" class="input">
                        <option v-for="c in categoryOrder" :key="c" :value="c">{{ categoryLabel(c) }}</option>
                    </select>
                </label>
                <label class="field">
                    <span>Save as</span>
                    <input v-model="filename" class="input mono" placeholder="from the link" />
                </label>
            </div>

            <label class="field">
                <span>Display name (optional)</span>
                <input v-model="name" class="input" placeholder="Shown on the Download list" />
            </label>

            <div v-if="info" class="info">
                <div><span class="faint">Saves to</span> <span class="mono">{{ info.path }}</span></div>
                <div>
                    <span class="faint">Size</span> {{ info.size ? formatBytes(info.size) : "unknown" }}
                    <span class="faint"> · Checksum</span> {{ info.sha256 ? "SHA-256 from Hugging Face" : "not available (file is not verified)" }}
                </div>
                <div v-if="info.exists" class="warn">A file with this name already exists.</div>
            </div>
            <p v-if="error" class="status-message status-message--error">{{ error }}</p>
            <p v-else-if="!info && isHuggingFace" class="faint small">Check looks up the size and checksum on Hugging Face.</p>

            <footer class="actions">
                <button class="btn-secondary" type="button" :disabled="busy" @click="check"><span v-if="busy && !info" class="spin" />{{ busy && !info ? "Checking…" : "Check" }}</button>
                <button class="btn" type="submit" :disabled="busy || !info || info.exists">
                    <span v-if="busy && info" class="spin" /><Icon v-else name="download" :size="16" />Add and download{{ info?.size ? ` ${formatBytes(info.size)}` : "" }}
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
    gap: 16px;
    width: min(620px, 100%);
    padding: 20px;
    max-height: calc(100dvh - 40px);
    overflow-y: auto;
}

.head {
    display: flex;
    align-items: center;
    justify-content: space-between;
}

.row {
    display: grid;
    grid-template-columns: 1fr 1fr;
    gap: 12px;
}

small,
.small {
    font-size: 12px;
    margin: 0;
}

.info {
    display: grid;
    gap: 4px;
    padding: 10px 12px;
    border-radius: var(--radius-sm);
    background: var(--bg-hover);
    font-size: 13px;
}

.warn {
    color: var(--warn);
}

.actions {
    display: flex;
    justify-content: flex-end;
    gap: 8px;
}

@media (max-width: 600px) {
    .row {
        grid-template-columns: 1fr;
    }
}
</style>
