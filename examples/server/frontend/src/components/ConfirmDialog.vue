<script setup lang="ts">
import { onBeforeUnmount, onMounted } from "vue";

import { answer, confirmState } from "../lib/confirm";

function onKey(event: KeyboardEvent) {
    if (event.key === "Escape") answer(null);
}

onMounted(() => window.addEventListener("keydown", onKey));
onBeforeUnmount(() => window.removeEventListener("keydown", onKey));
</script>

<template>
    <div v-if="confirmState.request" class="overlay" role="alertdialog" aria-modal="true" aria-labelledby="confirm-title" @click.self="answer(null)">
        <div class="dialog panel">
            <h2 id="confirm-title">{{ confirmState.request.title }}</h2>
            <p class="message">{{ confirmState.request.message }}</p>
            <ul v-if="confirmState.request.details?.length" class="details">
                <li v-for="line in confirmState.request.details" :key="line" class="mono">{{ line }}</li>
            </ul>
            <p v-if="confirmState.request.warning" class="warning">{{ confirmState.request.warning }}</p>
            <footer class="actions">
                <button class="btn-ghost" type="button" @click="answer(null)">Cancel</button>
                <button
                    v-for="(choice, index) in confirmState.request.choices"
                    :key="choice.value"
                    type="button"
                    :class="choice.danger ? 'btn danger' : choice.primary ? 'btn' : 'btn-secondary'"
                    :autofocus="index === confirmState.request.choices.length - 1"
                    @click="answer(choice.value)"
                >
                    {{ choice.label }}
                </button>
            </footer>
        </div>
    </div>
</template>

<style scoped>
.overlay {
    position: fixed;
    inset: 0;
    z-index: 80;
    display: grid;
    place-items: center;
    padding: 20px;
    background: rgba(5, 5, 8, 0.78);
}

.dialog {
    display: grid;
    gap: 12px;
    width: min(520px, 100%);
    padding: 20px;
    max-height: calc(100dvh - 40px);
    overflow-y: auto;
}

.message {
    margin: 0;
    color: var(--text-2);
    line-height: 1.55;
}

.details {
    display: grid;
    gap: 4px;
    max-height: 220px;
    margin: 0;
    padding: 10px 12px;
    overflow-y: auto;
    list-style: none;
    border-radius: var(--radius-sm);
    background: var(--bg-sunken);
}

.warning {
    margin: 0;
    padding: 9px 12px;
    border-radius: var(--radius-sm);
    background: rgba(240, 184, 92, 0.12);
    color: var(--warn);
    font-size: 13px;
}

.actions {
    display: flex;
    flex-wrap: wrap;
    justify-content: flex-end;
    gap: 8px;
    margin-top: 4px;
}

.danger {
    background: var(--danger);
    color: #1a0a0c;
}

.danger:hover:not(:disabled) {
    background: #ff97a0;
}
</style>
