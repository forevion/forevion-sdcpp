<script setup lang="ts">
import { computed, onBeforeUnmount, ref, watch } from "vue";

import type { OutputEntry } from "../lib/types";

const props = defineProps<{ output: OutputEntry; thumb?: boolean }>();

const isVideo = computed(() => props.output.mime_type.startsWith("video/"));
// Clips with sound are AVI, which browsers cannot play; the server saves a silent WebM preview
// and a WAV of the soundtrack, which plays in step with the preview.
const playable = computed(() => props.output.preview_url || props.output.url);
const withSound = computed(() => !props.thumb && isVideo.value && Boolean(props.output.audio_url));

const SOUND_KEY = "sdcpp.studio.sound";
function readSoundPref() {
    try {
        return localStorage.getItem(SOUND_KEY) === "on";
    } catch {
        return false;
    }
}

const video = ref<HTMLVideoElement>();
const audio = ref<HTMLAudioElement>();
const soundOn = ref(readSoundPref());
const blocked = ref(false);

// Keeps the soundtrack on the preview's clock; a looping preview restarts it as well.
function sync(force = false) {
    const v = video.value;
    const a = audio.value;
    if (!v || !a) return;
    a.muted = !soundOn.value;
    a.playbackRate = v.playbackRate;
    const target = Math.min(v.currentTime, Number.isFinite(a.duration) ? a.duration : v.currentTime);
    if (force || Math.abs(a.currentTime - target) > 0.08) a.currentTime = target;
    if (v.paused || !soundOn.value) {
        a.pause();
    } else if (a.paused && !a.ended) {
        a.play().then(
            () => (blocked.value = false),
            () => (blocked.value = true),
        );
    }
}

function toggleSound() {
    soundOn.value = !soundOn.value;
    try {
        localStorage.setItem(SOUND_KEY, soundOn.value ? "on" : "off");
    } catch {
        // Private windows may refuse storage; the toggle still works for this page.
    }
    if (soundOn.value && video.value?.paused) video.value.play().catch(() => undefined);
    sync(true);
}

watch(
    () => props.output.audio_url,
    () => (blocked.value = false),
);
onBeforeUnmount(() => audio.value?.pause());
</script>

<template>
    <div class="media" :class="{ thumb }">
        <video
            v-if="isVideo"
            ref="video"
            :key="output.url"
            :src="playable"
            :controls="!thumb"
            :autoplay="!thumb"
            loop
            muted
            playsinline
            :preload="thumb ? 'metadata' : 'auto'"
            @play="sync()"
            @pause="sync()"
            @seeked="sync(true)"
            @ratechange="sync()"
            @timeupdate="sync()"
        />
        <img v-else :src="output.url" :alt="output.prompt" loading="lazy" />
        <audio v-if="withSound" ref="audio" :key="output.audio_url" :src="output.audio_url" preload="auto" @canplay="sync(true)" />
        <button
            v-if="withSound"
            type="button"
            class="sound toggle"
            :class="{ on: soundOn }"
            :aria-pressed="soundOn"
            :title="blocked ? 'The browser blocked playback; click again to play the sound' : soundOn ? 'Mute the sound' : 'Play the sound'"
            @click="toggleSound"
        >
            {{ soundOn && !blocked ? "♪ Sound on" : "♪ Sound off" }}
        </button>
        <span v-else-if="output.has_audio" class="sound" :title="thumb ? 'Has sound' : 'Preview is silent; Save downloads the AVI with sound'">{{ thumb ? "♪" : "♪ sound in the saved AVI" }}</span>
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

.sound {
    position: absolute;
    left: 6px;
    bottom: 6px;
    padding: 1px 6px;
    border-radius: 6px;
    background: rgba(0, 0, 0, 0.65);
    color: #fff;
    font-size: 11px;
    font-weight: 600;
}

.sound.toggle {
    border: 0;
    cursor: pointer;
    font: inherit;
    font-size: 11px;
    font-weight: 600;
}

.sound.toggle.on {
    background: var(--accent);
    color: var(--accent-text);
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
