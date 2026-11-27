import { computed } from "vue";

import { recipeReady, state } from "./store";
import type { DownloadPackage, Recipe } from "./types";

// What the Models page shows: one card per model. A model is a download package plus the
// variants (user recipes) whose main model file belongs to it; variants whose main file
// comes from no package form "your own" models, grouped by that file.

export type ModelState =
    | "downloading"
    | "partial"
    | "not-downloaded"
    | "needs-setup"
    | "ready"
    | "missing-files";

export interface ModelCard {
    key: string;
    kind: "package" | "own";
    name: string;
    description: string;
    notes: string[];
    pkg?: DownloadPackage;
    mainFile: string;
    variants: Recipe[];
    readyVariants: Recipe[];
    state: ModelState;
}

const ACTIVE = ["queued", "starting", "downloading", "verifying", "packing"];

export function mainFileOf(recipe: Recipe): string {
    return recipe.models?.diffusion_model || recipe.models?.model || "";
}

function packageFiles(pkg: DownloadPackage): Set<string> {
    const files = new Set(pkg.components.map((c) => c.path));
    for (const file of Object.values(pkg.guide?.inputs || {})) files.add(file);
    return files;
}

function packageState(pkg: DownloadPackage, ready: Recipe[]): ModelState {
    if (ACTIVE.includes(pkg.status.state)) return "downloading";
    if (!pkg.installed) return pkg.missing_bytes < pkg.size || ["failed", "cancelled"].includes(pkg.status.state) ? "partial" : "not-downloaded";
    return ready.length ? "ready" : "needs-setup";
}

export const modelCards = computed<ModelCard[]>(() => {
    const variants = state.recipes.filter((r) => r.source === "user");
    const claimed = new Set<string>();
    const cards: ModelCard[] = [];

    for (const pkg of state.downloads?.packages || []) {
        const files = packageFiles(pkg);
        const mine = variants.filter((r) => mainFileOf(r) && files.has(mainFileOf(r)));
        mine.forEach((r) => claimed.add(r.id));
        const ready = mine.filter(recipeReady);
        cards.push({
            key: `pkg:${pkg.id}`,
            kind: "package",
            name: pkg.name,
            description: pkg.description || "",
            notes: pkg.guide?.notes || [],
            pkg,
            mainFile: pkg.guide?.inputs?.diffusion_model || pkg.guide?.inputs?.model || "",
            variants: mine,
            readyVariants: ready,
            state: packageState(pkg, ready),
        });
    }

    const own = new Map<string, Recipe[]>();
    for (const recipe of variants) {
        if (claimed.has(recipe.id)) continue;
        const key = mainFileOf(recipe) || `recipe:${recipe.id}`;
        own.set(key, [...(own.get(key) || []), recipe]);
    }
    for (const [file, list] of own) {
        const ready = list.filter(recipeReady);
        cards.push({
            key: `own:${file}`,
            kind: "own",
            name: list.length === 1 ? list[0].name : file.split("/").pop() || list[0].name,
            description: list[0].description || `Your model using ${file.split("/").pop()}.`,
            notes: [],
            mainFile: file,
            variants: list,
            readyVariants: ready,
            state: ready.length ? "ready" : "missing-files",
        });
    }
    return cards;
});

export const stateLabels: Record<ModelState, string> = {
    downloading: "Downloading",
    partial: "Partly downloaded",
    "not-downloaded": "Not downloaded",
    "needs-setup": "Needs setup",
    ready: "Ready",
    "missing-files": "Missing files",
};

export function isActiveDownload(pkg?: DownloadPackage) {
    return Boolean(pkg && ACTIVE.includes(pkg.status.state));
}
