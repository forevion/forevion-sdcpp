<script setup lang="ts">
import { computed, onMounted, ref, watch } from "vue";

import ConfirmDialog from "./components/ConfirmDialog.vue";
import DglWorkspace from "./components/DglWorkspace.vue";
import FolderSetup from "./components/FolderSetup.vue";
import Icon from "./components/Icon.vue";
import QueuePanel from "./components/QueuePanel.vue";
import { api } from "./lib/api";
import { APP_NAME, logoSvg } from "./lib/brand";
import { navigation } from "./lib/modelActions";
import { activeJobs, enterCreate, loadCatalog, loadDownloads, loadOutputs, startPolling, state } from "./lib/store";
import CreateView from "./views/CreateView.vue";
import GalleryView from "./views/GalleryView.vue";
import ModelsView from "./views/ModelsView.vue";

type Page = "create" | "gallery" | "models" | "scripts";
const pages: { id: Page; label: string }[] = [
    { id: "create", label: "Create" },
    { id: "gallery", label: "Gallery" },
    { id: "models", label: "Models" },
    { id: "scripts", label: "Scripts" },
];

function initialPage(): Page {
    const hash = window.location.hash.replace("#", "") as Page;
    return pages.some((p) => p.id === hash) ? hash : "create";
}

const page = ref<Page>(initialPage());
function go(next: Page) {
    if (next === "create") enterCreate();
    page.value = next;
    history.replaceState(null, "", `#${next}`);
    if (next === "models") loadDownloads();
    if (next === "gallery") loadOutputs();
}

const runningCount = computed(() => activeJobs.value.length);
const logo = logoSvg;

// The tab shows the page and how many jobs are running, so a background tab still tells you.
watch(
    [page, runningCount],
    ([current, running]) => {
        const label = pages.find((p) => p.id === current)?.label || "";
        document.title = `${running ? `(${running} running) ` : ""}${label} · ${APP_NAME}`;
    },
    { immediate: true },
);
const downloading = computed(
    () => state.downloads?.packages.filter((p) => ["queued", "starting", "downloading", "verifying", "packing"].includes(p.status.state)).length ?? 0,
);

watch(
    () => navigation.page,
    (next) => {
        if (!next) return;
        navigation.page = "";
        // Opening Create for a chosen model keeps the prompt; only the model changes.
        if (next === "create") {
            page.value = "create";
            history.replaceState(null, "", "#create");
        } else go(next);
    },
);

// On first start (or when a saved folder is gone) the folders are chosen before anything else.
const needsSetup = ref(false);

async function start() {
    if (page.value === "create") enterCreate();
    await Promise.all([loadCatalog(), loadOutputs(), loadDownloads()]);
    startPolling();
}

function setupDone() {
    needsSetup.value = false;
    start();
}

onMounted(async () => {
    try {
        needsSetup.value = (await api.folders()).needs_setup;
    } catch {
        needsSetup.value = false;
    }
    if (!needsSetup.value) await start();
});
</script>

<template>
    <div v-if="needsSetup" class="first-run">
        <section class="first-run-card panel" aria-labelledby="first-run-title">
            <div class="first-run-brand"><span class="first-run-logo" aria-hidden="true" v-html="logo" /><h1 id="first-run-title">Welcome to {{ APP_NAME }}</h1></div>
            <FolderSetup first-run @saved="setupDone" />
        </section>
    </div>
    <div v-else class="shell">
        <nav class="rail" aria-label="Main">
            <div class="brand" :title="APP_NAME" aria-hidden="true" v-html="logo" />
            <button
                v-for="item in pages"
                :key="item.id"
                class="rail-item"
                type="button"
                :aria-current="page === item.id ? 'page' : undefined"
                @click="go(item.id)"
            >
                <Icon :name="item.id" :size="20" />
                <span>{{ item.label }}</span>
                <i v-if="item.id === 'models' && downloading" class="dot" />
            </button>
            <div class="rail-spacer" />
            <button class="rail-item" type="button" :aria-pressed="state.queueOpen" @click="state.queueOpen = !state.queueOpen">
                <Icon name="queue" :size="20" />
                <span>Queue</span>
                <b v-if="runningCount" class="badge">{{ runningCount }}</b>
            </button>
        </nav>

        <main class="main">
            <CreateView v-if="page === 'create'" @open-models="go('models')" @open-gallery="go('gallery')" />
            <GalleryView v-else-if="page === 'gallery'" @open-create="go('create')" />
            <ModelsView v-else-if="page === 'models'" />
            <div v-else class="scripts"><DglWorkspace /></div>
        </main>

        <QueuePanel v-if="state.queueOpen" @close="state.queueOpen = false" />
        <ConfirmDialog />
    </div>
</template>

<style scoped>
.first-run {
    height: 100%;
    overflow-y: auto;
    padding: 32px 16px;
}

.first-run-card {
    display: grid;
    gap: 18px;
    grid-template-columns: minmax(0, 1fr);
    width: min(720px, 100%);
    margin: 0 auto;
    padding: 24px;
}

.first-run-brand {
    display: flex;
    align-items: center;
    gap: 12px;
}

.first-run-brand h1 {
    margin: 0;
    font-size: 22px;
}

.first-run-logo {
    width: 40px;
    height: 40px;
}

.first-run-logo :deep(svg) {
    display: block;
    width: 100%;
    height: 100%;
}

.shell {
    display: grid;
    grid-template-columns: 76px minmax(0, 1fr) auto;
    height: 100%;
}

.rail {
    display: flex;
    flex-direction: column;
    align-items: center;
    gap: 4px;
    padding: 14px 8px;
    border-right: 1px solid var(--line);
    background: var(--bg-sunken);
}

.brand {
    width: 30px;
    height: 30px;
    margin-bottom: 14px;
}

.brand :deep(svg) {
    display: block;
    width: 100%;
    height: 100%;
}

.rail-item {
    position: relative;
    display: grid;
    justify-items: center;
    gap: 3px;
    width: 60px;
    padding: 9px 0 7px;
    border: 0;
    border-radius: 10px;
    background: transparent;
    color: var(--text-3);
    font-size: 11px;
    font-weight: 600;
    cursor: pointer;
}

.rail-item:hover {
    color: var(--text);
    background: var(--bg-hover);
}

.rail-item[aria-current="page"],
.rail-item[aria-pressed="true"] {
    color: var(--text);
    background: var(--bg-hover);
}

.rail-item[aria-current="page"]::before {
    content: "";
    position: absolute;
    left: -8px;
    top: 12px;
    bottom: 12px;
    width: 3px;
    border-radius: 3px;
    background: var(--accent);
}

.rail-spacer {
    flex: 1;
}

.badge {
    position: absolute;
    top: 4px;
    right: 10px;
    min-width: 17px;
    height: 17px;
    padding: 0 4px;
    border-radius: 9px;
    background: var(--accent);
    color: var(--accent-text);
    font-size: 10px;
    line-height: 17px;
}

.dot {
    position: absolute;
    top: 8px;
    right: 16px;
    width: 7px;
    height: 7px;
    border-radius: 50%;
    background: var(--accent);
    animation: pulse 1.6s ease-in-out infinite;
}

@keyframes pulse {
    50% {
        opacity: 0.3;
    }
}

.main {
    min-width: 0;
    overflow: auto;
}

.scripts {
    padding: 20px;
    height: 100%;
}

@media (max-width: 720px) {
    .shell {
        grid-template-columns: 1fr;
        grid-template-rows: minmax(0, 1fr) auto;
    }

    .rail {
        order: 2;
        flex-direction: row;
        justify-content: space-around;
        padding: 6px;
        border-right: 0;
        border-top: 1px solid var(--line);
    }

    .brand,
    .rail-spacer {
        display: none;
    }

    .rail-item[aria-current="page"]::before {
        display: none;
    }
}
</style>
