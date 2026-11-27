<script setup lang="ts">
import { computed, ref } from "vue";

import Icon from "./Icon.vue";
import PackageEditor from "./PackageEditor.vue";
import PresetEditor from "./PresetEditor.vue";
import { categoryLabel } from "../lib/categories";
import { busy, deletePackageFiles, deleteVariant, removePackage, startDownload, stopDownload, useModel } from "../lib/modelActions";
import { stateLabels } from "../lib/models";
import type { ModelCard } from "../lib/models";
import { formatBytes, recipeReady, state } from "../lib/store";
import type { Recipe } from "../lib/types";

const props = defineProps<{ card: ModelCard }>();
const emit = defineEmits<{ (e: "back"): void; (e: "setup"): void }>();

type Section = "about" | "variants" | "files" | "advanced";
const section = ref<Section>(props.card.state === "ready" ? "variants" : "about");
const editing = ref<{ mode: "new" | "edit" | "copy"; base?: Recipe; guidePackage?: string } | null>(null);
const editingPackage = ref(false);

const pkg = computed(() => props.card.pkg);
const fileIds = computed(() => new Set(state.files.map((f) => f.id)));
const ownFiles = computed(() => {
    const files = new Map<string, string[]>();
    for (const variant of props.card.variants)
        for (const [slot, file] of Object.entries(variant.models || {})) files.set(file, [...(files.get(file) || []), slot]);
    return [...files.entries()].map(([path, slots]) => ({ path, slots: [...new Set(slots)], installed: fileIds.value.has(path) }));
});
const installedCount = computed(() => (pkg.value ? pkg.value.components.filter((c) => c.installed).length : 0));
const sourceText = computed(() => {
    const p = pkg.value;
    if (!p) return "Made by you from files on this computer.";
    if (p.overrides) return `Edited by you (the ${p.overrides} original is kept; Reset brings it back).`;
    return { builtin: "Built into Forevion Studio.", online: "From the online catalog." }[p.source] || "Added by you.";
});
const canRemove = computed(() => Boolean(pkg.value && pkg.value.source === `${pkg.value.id}.json`));

function newVariant() {
    if (pkg.value?.guide?.inputs) editing.value = { mode: "new", guidePackage: pkg.value.id };
    else if (props.card.variants[0]) editing.value = { mode: "copy", base: props.card.variants[0] };
    else editing.value = { mode: "new" };
}

function variantInfo(recipe: Recipe) {
    const d = recipe.defaults || {};
    const decoder = recipe.models?.tae ? "tiny decoder" : recipe.models?.vae ? "full VAE" : "";
    return [d.width && d.height ? `${d.width}×${d.height}` : "", d.steps ? `${d.steps} steps` : "", d.video_frames ? `${d.video_frames} frames` : "", decoder]
        .filter(Boolean)
        .join(" · ");
}
</script>

<template>
    <section class="detail">
        <header class="head">
            <button class="btn-ghost icon-btn" type="button" aria-label="Back to all models" @click="emit('back')"><Icon name="chevron" style="transform: rotate(180deg)" /></button>
            <div class="title">
                <h1>{{ card.name }}</h1>
                <span class="status" :class="card.state">{{ stateLabels[card.state] }}</span>
            </div>
            <span class="spacer" />
            <button v-if="card.state === 'downloading'" class="btn-secondary" type="button" @click="stopDownload(pkg!)"><Icon name="stop" :size="16" />Stop download</button>
            <button v-else-if="card.state === 'not-downloaded' || card.state === 'partial'" class="btn" type="button" :disabled="busy[`download:${pkg!.id}`]" @click="startDownload(pkg!)">
                <span v-if="busy[`download:${pkg!.id}`]" class="spin" /><Icon v-else name="download" :size="16" />{{ card.state === "partial" ? "Resume" : "Download" }} {{ formatBytes(pkg!.missing_bytes) }}
            </button>
            <button v-else-if="card.state === 'needs-setup'" class="btn" type="button" @click="emit('setup')"><Icon name="settings" :size="16" />Set up</button>
            <button v-else-if="card.state === 'ready'" class="btn" type="button" @click="useModel(card)"><Icon name="play" :size="16" />Use</button>
        </header>

        <nav class="segmented tabs" role="tablist" aria-label="Model sections">
            <button type="button" role="tab" :aria-pressed="section === 'about'" @click="section = 'about'">About</button>
            <button type="button" role="tab" :aria-pressed="section === 'variants'" @click="section = 'variants'">Variants ({{ card.variants.length }})</button>
            <button type="button" role="tab" :aria-pressed="section === 'files'" @click="section = 'files'">Files</button>
            <button v-if="pkg" type="button" role="tab" :aria-pressed="section === 'advanced'" @click="section = 'advanced'">Advanced</button>
        </nav>

        <div v-if="section === 'about'" class="panel block">
            <p>{{ card.description }}</p>
            <ul v-if="card.notes.length" class="notes">
                <li v-for="note in card.notes" :key="note">{{ note }}</li>
            </ul>
            <dl class="facts">
                <template v-if="pkg">
                    <dt>Download size</dt>
                    <dd>{{ formatBytes(pkg.size) }}<template v-if="pkg.missing_bytes && pkg.missing_bytes < pkg.size"> ({{ formatBytes(pkg.missing_bytes) }} still needed)</template></dd>
                </template>
                <template v-if="pkg?.license">
                    <dt>License</dt>
                    <dd>{{ pkg.license }}</dd>
                </template>
                <template v-if="pkg?.homepage">
                    <dt>Model page</dt>
                    <dd><a :href="pkg.homepage" target="_blank" rel="noopener noreferrer">{{ pkg.homepage.replace("https://", "") }}</a></dd>
                </template>
                <dt>Source</dt>
                <dd>{{ sourceText }}</dd>
            </dl>
            <template v-if="pkg?.guide?.inputs">
                <h3>What it needs</h3>
                <dl class="facts">
                    <template v-for="(file, slot) in pkg.guide.inputs" :key="slot">
                        <dt>{{ String(slot).replace("_", " ") }}</dt>
                        <dd class="mono">{{ file.split("/").pop() }}</dd>
                    </template>
                </dl>
            </template>
        </div>

        <div v-else-if="section === 'variants'" class="block">
            <p class="faint small">A variant is a ready-to-use setup of this model: which file goes into each input and its default settings. Create lists every ready variant.</p>
            <div class="variants">
                <article v-for="v in card.variants" :key="v.id" class="variant panel">
                    <div class="row">
                        <strong>{{ v.name }}</strong>
                        <span v-if="recipeReady(v)" class="chip chip--ok"><Icon name="check" :size="14" />Ready</span>
                        <span v-else class="chip chip--danger">Missing files</span>
                    </div>
                    <p class="faint small">{{ variantInfo(v) }}</p>
                    <div class="row actions">
                        <button class="btn-secondary" type="button" :disabled="!recipeReady(v)" @click="useModel(card, v)"><Icon name="play" :size="16" />Use</button>
                        <button class="btn-ghost" type="button" @click="editing = { mode: 'edit', base: v }"><Icon name="edit" :size="16" />Edit</button>
                        <button class="btn-ghost" type="button" @click="editing = { mode: 'copy', base: v }"><Icon name="reuse" :size="16" />Copy</button>
                        <button class="btn-ghost btn-danger icon-btn" type="button" aria-label="Delete variant" :disabled="busy[`variant:${v.id}`]" @click="deleteVariant(v)">
                            <span v-if="busy[`variant:${v.id}`]" class="spin" /><Icon v-else name="trash" :size="16" />
                        </button>
                    </div>
                </article>
                <button class="variant new" type="button" :disabled="!!pkg && !pkg.installed && !card.variants.length" @click="newVariant">
                    <Icon name="create" :size="18" />New variant
                </button>
            </div>
            <p v-if="pkg && !pkg.installed && !card.variants.length" class="faint small">Download the model first.</p>
        </div>

        <div v-else-if="section === 'files'" class="block">
            <template v-if="pkg">
                <ul class="files panel">
                    <li v-for="c in pkg.components" :key="c.path">
                        <Icon :name="c.installed ? 'check' : 'download'" :size="14" :class="c.installed ? 'ok' : 'faint'" />
                        <span class="mono ellipsis">{{ c.path }}</span>
                        <span class="faint">{{ categoryLabel(c.path.split("/")[0]) }}</span>
                        <span>{{ formatBytes(c.size) }}{{ c.packed_from ? " (packed)" : "" }}</span>
                    </li>
                </ul>
                <div class="row">
                    <button
                        class="btn-ghost btn-danger"
                        type="button"
                        :disabled="!installedCount || card.state === 'downloading' || busy[`delete:${pkg.id}`]"
                        @click="deletePackageFiles(pkg)"
                    >
                        <span v-if="busy[`delete:${pkg.id}`]" class="spin" /><Icon v-else name="trash" :size="16" />Delete downloaded files
                    </button>
                </div>
            </template>
            <ul v-else class="files panel">
                <li v-for="f in ownFiles" :key="f.path">
                    <Icon :name="f.installed ? 'check' : 'close'" :size="14" :class="f.installed ? 'ok' : 'warn'" />
                    <span class="mono ellipsis">{{ f.path }}</span>
                    <span class="faint">{{ f.slots.join(", ") }}</span>
                    <span>{{ f.installed ? "on disk" : "missing" }}</span>
                </li>
            </ul>
        </div>

        <div v-else-if="pkg" class="panel block">
            <p class="faint small">{{ sourceText }} Editing changes the download links, file names, sizes and checksums of this model.</p>
            <div class="row">
                <button class="btn-secondary" type="button" @click="editingPackage = true"><Icon name="edit" :size="16" />Edit download details</button>
                <button v-if="canRemove" class="btn-ghost btn-danger" type="button" :disabled="busy[`remove:${pkg.id}`]" @click="removePackage(pkg)">
                    <span v-if="busy[`remove:${pkg.id}`]" class="spin" />{{ pkg.overrides ? "Reset to original" : "Remove from the list" }}
                </button>
            </div>
        </div>

        <PresetEditor v-if="editing" :mode="editing.mode" :base="editing.base" :guide-package="editing.guidePackage" @close="editing = null" />
        <PackageEditor v-if="editingPackage && pkg" :package-id="pkg.id" @close="editingPackage = false" />
    </section>
</template>

<style scoped>
.detail {
    display: grid;
    gap: 16px;
}

.head {
    display: flex;
    align-items: center;
    flex-wrap: wrap;
    gap: 10px;
}

.title {
    display: flex;
    align-items: center;
    gap: 10px;
}

.spacer {
    flex: 1;
}

.status {
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

.tabs {
    justify-self: start;
    min-width: min(520px, 100%);
}

.block {
    display: grid;
    gap: 12px;
    padding: 16px;
}

.block:not(.panel) {
    padding: 0;
}

.block p {
    margin: 0;
}

h3 {
    color: var(--text-2);
    font-size: 12px;
    text-transform: uppercase;
    letter-spacing: 0.08em;
}

.notes {
    display: grid;
    gap: 4px;
    margin: 0;
    padding-left: 18px;
    color: var(--text-2);
}

.facts {
    display: grid;
    grid-template-columns: auto minmax(0, 1fr);
    gap: 6px 16px;
    margin: 0;
    font-size: 13px;
}

.facts dt {
    color: var(--text-3);
    text-transform: capitalize;
}

.facts dd {
    margin: 0;
    word-break: break-all;
}

.small {
    font-size: 12px;
}

.variants {
    display: grid;
    grid-template-columns: repeat(auto-fill, minmax(280px, 1fr));
    gap: 12px;
}

.variant {
    display: grid;
    align-content: start;
    gap: 8px;
    padding: 14px;
}

.variant.new {
    display: grid;
    place-items: center;
    align-content: center;
    gap: 6px;
    min-height: 120px;
    border: 1px dashed var(--line-strong);
    border-radius: var(--radius);
    background: transparent;
    color: var(--text-2);
    cursor: pointer;
}

.variant.new:hover:not(:disabled) {
    border-color: var(--accent);
    color: var(--text);
}

.row {
    display: flex;
    flex-wrap: wrap;
    align-items: center;
    justify-content: space-between;
    gap: 8px;
}

.row.actions {
    justify-content: flex-start;
    gap: 4px;
}

.files {
    display: grid;
    gap: 8px;
    margin: 0;
    padding: 14px;
    list-style: none;
}

.files li {
    display: grid;
    grid-template-columns: auto minmax(0, 1fr) auto auto;
    align-items: center;
    gap: 12px;
    font-size: 13px;
}

.ellipsis {
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
}

.ok {
    color: var(--ok);
}

.warn {
    color: var(--warn);
}
</style>
