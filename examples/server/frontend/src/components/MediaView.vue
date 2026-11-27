<script setup lang="ts">
import { computed } from "vue";

import type { OutputEntry } from "../lib/types";

const props = defineProps<{ output: OutputEntry; thumb?: boolean }>();

const isVideo = computed(() => props.output.mime_type.startsWith("video/"));
</script>

<template>
    <div class="media" :class="{ thumb }">
        <video
            v-if="isVideo"
            :key="output.url"
            :src="output.url"
            :controls="!thumb"
            :autoplay="!thumb"
            loop
            muted
            playsinline
            :preload="thumb ? 'metadata' : 'auto'"
        />
        <img v-else :src="output.url" :alt="output.prompt" loading="lazy" />
        <span v-if="thumb && isVideo" class="badge">{{ ((output.frame_count || 0) / (output.fps || 24)).toFixed(1) }}s</span>
    </div>
</template>

<style scoped>
.media {
    position: relative;
    display: grid;
    place-items: center;
    width: 100%;
    height: 100%;
    background: var(--bg-sunken);
}

.media video,
.media img {
    display: block;
    max-width: 100%;
    max-height: 64vh;
}

.media.thumb video,
.media.thumb img {
    width: 100%;
    height: 100%;
    max-height: none;
    object-fit: cover;
}

.badge {
    position: absolute;
    right: 6px;
    bottom: 6px;
    padding: 1px 6px;
    border-radius: 6px;
    background: rgba(0, 0, 0, 0.65);
    color: #fff;
    font-size: 11px;
    font-weight: 600;
}
</style>
