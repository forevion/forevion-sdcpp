<script setup lang="ts">
import { ref } from "vue";

import Icon from "./Icon.vue";

// Picks images by click or drag-and-drop and returns them as data URLs.
const props = defineProps<{ images: { name: string; data: string }[]; multiple?: boolean; disabled?: boolean; max?: number }>();
const emit = defineEmits<{ (e: "add", image: { name: string; data: string }): void; (e: "remove", index: number): void }>();

const input = ref<HTMLInputElement | null>(null);
const dragging = ref(false);
const error = ref("");

function readFile(file: File) {
    if (!file.type.startsWith("image/")) {
        error.value = `${file.name} is not an image.`;
        return;
    }
    if (file.size > 25 * 1024 * 1024) {
        error.value = `${file.name} is larger than 25 MB.`;
        return;
    }
    const reader = new FileReader();
    reader.onload = () => emit("add", { name: file.name, data: String(reader.result) });
    reader.onerror = () => (error.value = `Cannot read ${file.name}.`);
    reader.readAsDataURL(file);
}

function addFiles(list: FileList | null) {
    error.value = "";
    if (!list) return;
    const room = (props.max ?? (props.multiple ? 8 : 1)) - (props.multiple ? props.images.length : 0);
    [...list].slice(0, Math.max(0, room)).forEach(readFile);
}

function onDrop(event: DragEvent) {
    dragging.value = false;
    if (!props.disabled) addFiles(event.dataTransfer?.files || null);
}
</script>

<template>
    <div class="image-input">
        <div class="thumbs">
            <figure v-for="(image, index) in images" :key="image.name + index" class="thumb">
                <img :src="image.data" :alt="image.name" />
                <button class="remove" type="button" :aria-label="`Remove ${image.name}`" @click="emit('remove', index)"><Icon name="close" :size="14" /></button>
                <figcaption class="mono">{{ image.name }}</figcaption>
            </figure>
            <button
                v-if="multiple || !images.length"
                class="drop"
                :class="{ dragging }"
                type="button"
                :disabled="disabled"
                @click="input?.click()"
                @dragover.prevent="dragging = true"
                @dragleave="dragging = false"
                @drop.prevent="onDrop"
            >
                <Icon name="image" :size="20" />
                <span>{{ multiple ? "Add images" : "Choose an image" }}</span>
                <small class="faint">or drop here</small>
            </button>
        </div>
        <input ref="input" class="sr-only" type="file" accept="image/*" :multiple="multiple" @change="addFiles(($event.target as HTMLInputElement).files); ($event.target as HTMLInputElement).value = ''" />
        <p v-if="error" class="err">{{ error }}</p>
    </div>
</template>

<style scoped>
.image-input {
    display: grid;
    gap: 6px;
}

.thumbs {
    display: flex;
    flex-wrap: wrap;
    gap: 8px;
}

.thumb {
    position: relative;
    display: grid;
    gap: 4px;
    width: 104px;
    margin: 0;
}

.thumb img {
    width: 104px;
    height: 72px;
    object-fit: cover;
    border-radius: 8px;
    border: 1px solid var(--line);
}

.thumb figcaption {
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
    font-size: 11px;
    color: var(--text-3);
}

.remove {
    position: absolute;
    top: 4px;
    right: 4px;
    display: grid;
    place-items: center;
    width: 22px;
    height: 22px;
    border: 0;
    border-radius: 50%;
    background: rgba(0, 0, 0, 0.7);
    color: #fff;
    cursor: pointer;
}

.drop {
    display: grid;
    place-items: center;
    align-content: center;
    gap: 2px;
    width: 104px;
    height: 72px;
    border: 1px dashed var(--line-strong);
    border-radius: 8px;
    background: var(--bg-sunken);
    color: var(--text-2);
    font-size: 12px;
    cursor: pointer;
}

.drop:hover:not(:disabled),
.drop.dragging {
    border-color: var(--accent);
    color: var(--text);
}

.drop small {
    font-size: 10px;
}

.err {
    margin: 0;
    color: var(--danger);
    font-size: 12px;
}
</style>
