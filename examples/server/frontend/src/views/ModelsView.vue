<script setup lang="ts">
import { computed, onMounted, ref } from "vue";

import AddOwnModel from "../components/AddOwnModel.vue";
import Icon from "../components/Icon.vue";
import ModelDetail from "../components/ModelDetail.vue";
import ModelSettings from "../components/ModelSettings.vue";
import PresetEditor from "../components/PresetEditor.vue";
import { busy, startDownload, stopDownload, useModel } from "../lib/modelActions";
import { modelCards, stateLabels } from "../lib/models";
import type { ModelCard } from "../lib/models";
import { eta, formatBytes, loadDownloads, state } from "../lib/store";
import { capabilityBadges } from "../lib/tasks";

const selectedKey = ref("");
const settingsOpen = ref(false);
const addingOwn = ref(false);
const settingUp = ref("");

const selected = computed(() => modelCards.value.find((c) => c.key === selectedKey.value) || null);
const packageCards = computed(() => modelCards.value.filter((c) => c.kind === "package"));
const ownCards = computed(() => modelCards.value.filter((c) => c.kind === "own"));

function progress(card: ModelCard) {
    const s = card.pkg?.status;
    return s && s.total_bytes ? Math.min(1, s.done_bytes / s.total_bytes) : 0;
}

function downloadSeconds(card: ModelCard) {
    const s = card.pkg?.status;
    return s && s.speed_bps > 1024 ? (s.total_bytes - s.done_bytes) / s.speed_bps : null;
}

const stageText: Record<string, string> = {
    queued: "Waiting in line",
    starting: "Starting",
    downloading: "Downloading",
    verifying: "Checking the file",
    packing: "Packing to GGUF (a few minutes)",
};

// What a model can do: from a variant's resolved features, or the download guide's settings.
function badges(card: ModelCard) {
    const settings = card.pkg?.guide?.settings;
    return capabilityBadges(card.variants[0], {
        features: settings?.features,
        video: card.variants[0] ? undefined : Boolean(settings?.video_constraints || settings?.defaults?.video_frames),
        audio: Boolean(card.pkg?.guide?.inputs?.audio_vae),
    });
}

function summary(card: ModelCard) {
    const parts: string[] = [];
    if (card.variants.length) parts.push(`${card.variants.length} variant${card.variants.length > 1 ? "s" : ""}`);
    if (card.pkg) parts.push(formatBytes(card.pkg.size));
    return parts.join(" · ");
}

onMounted(loadDownloads);
</script>

<template>
    <div class="models-page">
        <template v-if="selected">
            <ModelDetail :card="selected" @back="selectedKey = ''" @setup="settingUp = selected.pkg?.id || ''" />
        </template>

        <template v-else>
            <header class="head">
                <div>
                    <h1>Models</h1>
                    <p class="muted">Download a model once, set it up, then use it on Create.</p>
                </div>
                <span class="spacer" />
                <button class="btn-secondary" type="button" @click="settingsOpen = true"><Icon name="settings" :size="16" />Settings</button>
            </header>

            <p v-if="state.loading.downloads || state.loading.catalog" class="loading-row"><span class="spin" />Loading models…</p>
            <p v-if="state.error" class="status-message status-message--error">{{ state.error }}</p>

            <div class="cards">
                <article
                    v-for="card in [...packageCards, ...ownCards]"
                    :key="card.key"
                    class="card panel clickable"
                    :class="card.state"
                    tabindex="0"
                    @click="selectedKey = card.key"
                    @keydown.enter="selectedKey = card.key"
                >
                    <div class="card-head">
                        <h3>{{ card.name }}</h3>
                        <span class="status" :class="card.state">{{ stateLabels[card.state] }}</span>
                    </div>
                    <p class="desc">{{ card.description }}</p>
                    <div class="badges">
                        <span v-for="badge in badges(card)" :key="badge" class="badge">{{ badge }}</span>
                    </div>
                    <p class="meta faint">{{ summary(card) }}<template v-if="card.kind === 'own'"> · your model</template></p>

                    <div v-if="card.state === 'downloading' && card.pkg" class="dl">
                        <div class="progress" :class="{ 'progress--indeterminate': ['packing', 'verifying', 'queued', 'starting'].includes(card.pkg.status.state) }">
                            <div :style="{ width: `${progress(card) * 100}%` }" />
                        </div>
                        <p class="faint small">
                            {{ stageText[card.pkg.status.state] || card.pkg.status.state }}
                            <template v-if="card.pkg.status.state === 'downloading'">
                                · {{ formatBytes(card.pkg.status.done_bytes) }} / {{ formatBytes(card.pkg.status.total_bytes) }} · {{ eta(downloadSeconds(card)) }}
                            </template>
                        </p>
                    </div>
                    <p v-if="card.pkg?.status.state === 'failed'" class="status-message status-message--error small">{{ card.pkg.status.error }}</p>

                    <div class="actions" @click.stop>
                        <button v-if="card.state === 'downloading'" class="btn-secondary" type="button" :disabled="busy[`stop:${card.pkg!.id}`]" @click="stopDownload(card.pkg!)">
                            <span v-if="busy[`stop:${card.pkg!.id}`]" class="spin" /><Icon v-else name="stop" :size="16" />Stop
                        </button>
                        <button
                            v-else-if="card.state === 'not-downloaded' || card.state === 'partial'"
                            class="btn"
                            type="button"
                            :disabled="busy[`download:${card.pkg!.id}`]"
                            @click="startDownload(card.pkg!)"
                        >
                            <span v-if="busy[`download:${card.pkg!.id}`]" class="spin" /><Icon v-else name="download" :size="16" />
                            {{ card.state === "partial" ? "Resume" : "Download" }} {{ formatBytes(card.pkg!.missing_bytes) }}
                        </button>
                        <button v-else-if="card.state === 'needs-setup'" class="btn" type="button" @click="settingUp = card.pkg!.id"><Icon name="settings" :size="16" />Set up</button>
                        <button v-else-if="card.state === 'ready'" class="btn" type="button" @click="useModel(card)"><Icon name="play" :size="16" />Use</button>
                        <button v-else class="btn-secondary" type="button" @click="selectedKey = card.key">Fix files</button>
                        <button class="btn-ghost" type="button" @click="selectedKey = card.key">Details<Icon name="chevron" :size="14" /></button>
                    </div>
                </article>

                <button class="card add-own" type="button" @click="addingOwn = true">
                    <Icon name="create" :size="22" />
                    <strong>Add your own model</strong>
                    <span class="faint small">From files you have or download links</span>
                </button>
            </div>
        </template>

        <ModelSettings v-if="settingsOpen" @close="settingsOpen = false" />
        <AddOwnModel v-if="addingOwn" @close="addingOwn = false" />
        <PresetEditor v-if="settingUp" mode="new" :guide-package="settingUp" @close="settingUp = ''" />
    </div>
</template>

<style scoped>
.models-page {
    display: grid;
    align-content: start;
    gap: 20px;
    padding: 22px;
    max-width: 1200px;
}

.head {
    display: flex;
    align-items: center;
    flex-wrap: wrap;
    gap: 12px;
}

.head p {
    margin: 4px 0 0;
}

.spacer {
    flex: 1;
}

.cards {
    display: grid;
    grid-template-columns: repeat(auto-fill, minmax(300px, 1fr));
    gap: 14px;
}

.card {
    display: flex;
    flex-direction: column;
    gap: 10px;
    padding: 16px;
    text-align: left;
}

.card.clickable {
    cursor: pointer;
    transition: border-color 0.12s;
}

.card.clickable:hover {
    border-color: var(--line-strong);
}

.card.needs-setup {
    border-color: var(--accent);
}

.card-head {
    display: flex;
    align-items: center;
    justify-content: space-between;
    gap: 8px;
}

.status {
    flex: none;
    padding: 2px 9px;
    border-radius: 999px;
    background: var(--bg-hover);
    color: var(--text-2);
    font-size: 12px;
    font-weight: 600;
}

.status.ready {
    color: var(--ok);
    background: rgba(95, 211, 154, 0.1);
}

.status.needs-setup,
.status.downloading {
    color: var(--accent-strong);
    background: var(--accent-soft);
}

.status.partial,
.status.missing-files {
    color: var(--warn);
    background: rgba(240, 184, 92, 0.12);
}

.desc {
    margin: 0;
    color: var(--text-2);
    display: -webkit-box;
    -webkit-line-clamp: 3;
    -webkit-box-orient: vertical;
    overflow: hidden;
}

.badges {
    display: flex;
    flex-wrap: wrap;
    gap: 5px;
}

.badge {
    padding: 1px 8px;
    border-radius: 6px;
    background: var(--bg-hover);
    color: var(--text-2);
    font-size: 11px;
    font-weight: 600;
}

.meta,
.small {
    margin: 0;
    font-size: 12px;
}

.dl {
    display: grid;
    gap: 6px;
}

.actions {
    display: flex;
    flex-wrap: wrap;
    gap: 8px;
    margin-top: auto;
    cursor: default;
}

.add-own {
    display: grid;
    place-items: center;
    align-content: center;
    gap: 6px;
    min-height: 190px;
    border: 1px dashed var(--line-strong);
    border-radius: var(--radius);
    background: transparent;
    color: var(--text-2);
    cursor: pointer;
}

.add-own:hover {
    border-color: var(--accent);
    color: var(--text);
}
</style>
