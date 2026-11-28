<script setup lang="ts">
import { ref } from "vue";

import Icon from "./Icon.vue";

// Picks an audio file (wav, mp3, m4a, ogg, …) and converts it to 16-bit PCM WAV in the
// browser, because the server reads reference audio as WAV only.
const props = defineProps<{ audio: { name: string; data: string; seconds: number } | null; maxSeconds?: number }>();
const emit = defineEmits<{ (e: "set", audio: { name: string; data: string; seconds: number } | null): void }>();

const input = ref<HTMLInputElement | null>(null);
const busy = ref(false);
const error = ref("");

function encodeWav(buffer: AudioBuffer, seconds: number): string {
    const channels = Math.min(2, buffer.numberOfChannels);
    const rate = buffer.sampleRate;
    const frames = Math.min(buffer.length, Math.floor(seconds * rate));
    const bytes = new DataView(new ArrayBuffer(44 + frames * channels * 2));
    const text = (offset: number, value: string) => [...value].forEach((c, i) => bytes.setUint8(offset + i, c.charCodeAt(0)));
    text(0, "RIFF");
    bytes.setUint32(4, 36 + frames * channels * 2, true);
    text(8, "WAVE");
    text(12, "fmt ");
    bytes.setUint32(16, 16, true);
    bytes.setUint16(20, 1, true);
    bytes.setUint16(22, channels, true);
    bytes.setUint32(24, rate, true);
    bytes.setUint32(28, rate * channels * 2, true);
    bytes.setUint16(32, channels * 2, true);
    bytes.setUint16(34, 16, true);
    text(36, "data");
    bytes.setUint32(40, frames * channels * 2, true);
    const data = [...Array(channels)].map((_, c) => buffer.getChannelData(c));
    let offset = 44;
    for (let i = 0; i < frames; i++)
        for (let c = 0; c < channels; c++) {
            const sample = Math.max(-1, Math.min(1, data[c][i]));
            bytes.setInt16(offset, sample < 0 ? sample * 0x8000 : sample * 0x7fff, true);
            offset += 2;
        }
    let binary = "";
    const raw = new Uint8Array(bytes.buffer);
    for (let i = 0; i < raw.length; i += 0x8000) binary += String.fromCharCode(...raw.subarray(i, i + 0x8000));
    return `data:audio/wav;base64,${btoa(binary)}`;
}

async function pick(list: FileList | null) {
    error.value = "";
    const file = list?.[0];
    if (!file) return;
    if (file.size > 100 * 1024 * 1024) {
        error.value = `${file.name} is larger than 100 MB.`;
        return;
    }
    busy.value = true;
    try {
        const context = new AudioContext();
        const decoded = await context.decodeAudioData(await file.arrayBuffer());
        await context.close();
        const limit = props.maxSeconds ?? 30;
        const seconds = Math.min(decoded.duration, limit);
        emit("set", { name: file.name, data: encodeWav(decoded, seconds), seconds });
        if (decoded.duration > limit) error.value = `Only the first ${limit} s are used.`;
    } catch {
        error.value = `Cannot read ${file.name} as audio.`;
    } finally {
        busy.value = false;
    }
}
</script>

<template>
    <div class="audio-input">
        <div v-if="audio" class="clip">
            <Icon name="play" :size="16" />
            <audio :src="audio.data" controls preload="metadata" />
            <span class="mono faint">{{ audio.name }} · {{ audio.seconds.toFixed(1) }} s</span>
            <button class="btn-ghost icon-btn" type="button" :aria-label="`Remove ${audio.name}`" @click="emit('set', null)"><Icon name="close" :size="14" /></button>
        </div>
        <button v-else class="btn-secondary" type="button" :disabled="busy" @click="input?.click()">
            <span v-if="busy" class="spin" /><Icon v-else name="download" :size="16" />{{ busy ? "Reading audio…" : "Choose an audio file" }}
        </button>
        <input ref="input" class="sr-only" type="file" accept="audio/*" @change="pick(($event.target as HTMLInputElement).files); ($event.target as HTMLInputElement).value = ''" />
        <p v-if="error" class="err">{{ error }}</p>
    </div>
</template>

<style scoped>
.audio-input {
    display: grid;
    gap: 6px;
    justify-items: start;
}

.clip {
    display: flex;
    flex-wrap: wrap;
    align-items: center;
    gap: 8px;
}

.clip audio {
    height: 32px;
    max-width: 240px;
}

.clip .mono {
    font-size: 11px;
}

.err {
    margin: 0;
    color: var(--warn);
    font-size: 12px;
}
</style>
