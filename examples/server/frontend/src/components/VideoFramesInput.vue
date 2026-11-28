<script setup lang="ts">
import { ref } from "vue";

import Icon from "./Icon.vue";

// Picks a video and extracts `frames` frames at `fps` (as JPEG data URLs) for control-video
// tasks; the server takes control videos as frame lists.
const props = defineProps<{ video: { name: string; frames: string[] } | null; frames: number; fps: number; width: number; height: number }>();
const emit = defineEmits<{ (e: "set", video: { name: string; frames: string[] } | null): void }>();

const input = ref<HTMLInputElement | null>(null);
const busy = ref("");
const error = ref("");

function seek(video: HTMLVideoElement, time: number) {
    return new Promise<void>((resolve, reject) => {
        video.onseeked = () => resolve();
        video.onerror = () => reject(new Error("seek failed"));
        video.currentTime = time;
    });
}

async function pick(list: FileList | null) {
    error.value = "";
    const file = list?.[0];
    if (!file) return;
    const url = URL.createObjectURL(file);
    const video = document.createElement("video");
    video.muted = true;
    video.src = url;
    try {
        await new Promise<void>((resolve, reject) => {
            video.onloadeddata = () => resolve();
            video.onerror = () => reject(new Error("unsupported video"));
        });
        const canvas = document.createElement("canvas");
        canvas.width = props.width;
        canvas.height = props.height;
        const context = canvas.getContext("2d")!;
        const frames: string[] = [];
        const count = Math.max(1, props.frames);
        for (let i = 0; i < count; i++) {
            const time = Math.min(video.duration - 0.001, i / props.fps);
            busy.value = `Reading frame ${i + 1} of ${count}…`;
            await seek(video, Math.max(0, time));
            // Cover-crop to the output size so the control video lines up with the result.
            const scale = Math.max(canvas.width / video.videoWidth, canvas.height / video.videoHeight);
            const w = video.videoWidth * scale;
            const h = video.videoHeight * scale;
            context.drawImage(video, (canvas.width - w) / 2, (canvas.height - h) / 2, w, h);
            frames.push(canvas.toDataURL("image/jpeg", 0.92));
        }
        emit("set", { name: file.name, frames });
        if (video.duration < (count - 1) / props.fps) error.value = "The video is shorter than the clip; its last frame repeats.";
    } catch {
        error.value = `Cannot read ${file.name} as a video in this browser.`;
    } finally {
        URL.revokeObjectURL(url);
        busy.value = "";
    }
}
</script>

<template>
    <div class="video-input">
        <div v-if="video" class="strip">
            <img v-for="(frame, index) in video.frames.filter((_, i) => i % Math.ceil(video!.frames.length / 6) === 0)" :key="index" :src="frame" alt="" />
            <span class="mono faint">{{ video.name }} · {{ video.frames.length }} frames</span>
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
