<script setup lang="ts">
import { computed, ref } from "vue";

import Icon from "./Icon.vue";
import { commandPreview } from "../lib/command";

const emit = defineEmits<{ (e: "close"): void }>();

const tab = ref<"cli" | "api">("cli");
const copied = ref("");
const preview = computed(() => commandPreview());

async function copy(text: string, what: string) {
    try {
        await navigator.clipboard.writeText(text);
        copied.value = what;
        setTimeout(() => (copied.value = ""), 1500);
    } catch {
        copied.value = "";
    }
}
</script>

<template>
    <div class="overlay" role="dialog" aria-modal="true" aria-labelledby="command-title" @click.self="emit('close')">
        <div class="dialog panel">
            <header class="head">
                <div>
                    <h2 id="command-title">Command for this generation</h2>
                    <p class="faint small">The same job as the form, for the terminal ({{ preview?.windows ? "Windows PowerShell" : "Linux shell" }}) or the server API.</p>
                </div>
                <button class="btn-ghost icon-btn" type="button" aria-label="Close" @click="emit('close')"><Icon name="close" /></button>
            </header>

            <div class="segmented tabs" role="tablist">
                <button type="button" role="tab" :aria-pressed="tab === 'cli'" @click="tab = 'cli'">sd-cli command</button>
                <button type="button" role="tab" :aria-pressed="tab === 'api'" @click="tab = 'api'">API request (what the UI sends)</button>
            </div>

            <template v-if="preview">
                <section v-if="tab === 'cli'" class="block">
                    <p class="faint small">Run it from the folder that contains sd-cli. It loads the models itself, so it does not need the server.</p>
                    <pre class="code mono">{{ preview.cli }}</pre>
                    <button class="btn-secondary" type="button" @click="copy(preview.cli, 'cli')">
                        <Icon :name="copied === 'cli' ? 'check' : 'download'" :size="16" />{{ copied === "cli" ? "Copied" : "Copy command" }}
                    </button>
                </section>
                <section v-else class="block">
                    <p class="faint small">POST {{ preview.endpoint }}, then poll /sdcpp/v1/jobs/&lt;id&gt; until it finishes. Save the JSON below as request.json:</p>
                    <pre class="code mono">{{ preview.requestJson }}</pre>
                    <div class="row">
                        <button class="btn-secondary" type="button" @click="copy(preview.requestJson, 'json')">
                            <Icon :name="copied === 'json' ? 'check' : 'download'" :size="16" />{{ copied === "json" ? "Copied" : "Copy JSON" }}
                        </button>
                    </div>
                    <pre class="code mono">{{ preview.curl }}</pre>
                    <button class="btn-secondary" type="button" @click="copy(preview.curl, 'curl')">
                        <Icon :name="copied === 'curl' ? 'check' : 'download'" :size="16" />{{ copied === "curl" ? "Copied" : "Copy curl" }}
                    </button>
                </section>
                <ul v-if="preview.notes.length" class="notes">
                    <li v-for="note in preview.notes" :key="note">{{ note }}</li>
                </ul>
            </template>
            <p v-else class="faint">Choose a model first.</p>
        </div>
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
    gap: 14px;
    width: min(860px, 100%);
    max-height: 100%;
    padding: 20px;
    overflow-y: auto;
}

.head {
    display: flex;
    align-items: flex-start;
    justify-content: space-between;
}

.head p,
.small {
    margin: 2px 0 0;
    font-size: 12px;
}

.tabs {
    justify-self: start;
}

.block {
    display: grid;
    gap: 10px;
    justify-items: start;
}

.code {
    width: 100%;
    max-height: 340px;
    margin: 0;
    padding: 12px 14px;
    overflow: auto;
    border-radius: var(--radius-sm);
    background: var(--bg-sunken);
    border: 1px solid var(--line);
    white-space: pre;
    line-height: 1.6;
}

.notes {
    display: grid;
    gap: 4px;
    margin: 0;
    padding-left: 18px;
    color: var(--text-3);
    font-size: 12px;
}
</style>
