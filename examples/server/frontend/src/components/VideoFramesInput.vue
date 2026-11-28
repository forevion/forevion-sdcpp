<script setup lang="ts">
import { ref } from "vue";

import Icon from "./Icon.vue";
import { readVideoFrames } from "../lib/frames";

// Picks a video and extracts `frames` frames at `fps` (as JPEG data URLs) for control-video
// tasks; the server takes control videos as frame lists. `native` keeps the video's own size
// (rounded down to 16, at most `maxSide`) and stops at the end of the video.
export interface PickedVideo {
    name: string;
    frames: string[];
    width?: number;
    height?: number;
}
const props = defineProps<{ video: PickedVideo | null; frames: number; fps: number; width: number; height: number; native?: boolean; maxSide?: number }>();
const emit = defineEmits<{ (e: "set", video: PickedVideo | null): void }>();

const input = ref<HTMLInputElement | null>(null);
const busy = ref("");
const error = ref("");

async function pick(list: FileList | null) {
    error.value = "";
    const file = list?.[0];
    if (!file) return;
    try {
        const clip = await readVideoFrames(file, {
            fps: props.fps,
            frames: props.frames,
            native: props.native,
            maxSide: props.maxSide,
            ...(props.native ? {} : { width: props.width, height: props.height }),
            onProgress: (done, total) => (busy.value = `Reading frame ${done + 1} of ${total}…`),
        });
        emit("set", { name: file.name, frames: clip.frames, width: clip.width, height: clip.height });
        if (clip.short) error.value = "The video is shorter than the clip; its last frame repeats.";
    } catch {
        error.value = `Cannot read ${file.name} as a video in this browser.`;
    } finally {
        busy.value = "";
    }
}
</script>

<template>
    <div class="video-input">
        <div v-if="video" class="strip">
            <img v-for="(frame, index) in video.frames.filter((_, i) => i % Math.ceil(video!.frames.length / 6) === 0)" :key="index" :src="frame" alt="" />
            <span class="mono faint">{{ video.name }} · {{ video.frames.length }} frames<template v-if="video.width"> · {{ video.width }}×{{ video.height }}</template></span>
            <button class="btn-ghost icon-btn" type="button" :aria-label="`Remove ${video.name}`" @click="emit('set', null)"><Icon name="close" :size="14" /></button>
        </div>
        <button v-else class="btn-secondary" type="button" :disabled="!!busy" @click="input?.click()">
            <span v-if="busy" class="spin" /><Icon v-else name="video" :size="16" />{{ busy || "Choose a video" }}
        </button>
        <input ref="input" class="sr-only" type="file" accept="video/*" @change="pick(($event.target as HTMLInputElement).files); ($event.target as HTMLInputElement).value = ''" />
        <p v-if="error" class="err">{{ error }}</p>
    </div>
</template>

<style scoped>
.video-input {
    display: grid;
    gap: 6px;
    justify-items: start;
}

.strip {
    display: flex;
    flex-wrap: wrap;
    align-items: center;
    gap: 6px;
}

.strip img {
    width: 64px;
    height: 40px;
    object-fit: cover;
    border-radius: 5px;
    border: 1px solid var(--line);
}

.strip .mono {
    font-size: 11px;
}

.err {
    margin: 0;
    color: var(--warn);
    font-size: 12px;
}
</style>
