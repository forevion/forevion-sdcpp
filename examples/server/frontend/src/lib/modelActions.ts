import { reactive } from "vue";

import { api } from "./api";
import { confirmDanger } from "./confirm";
import { formatBytes, loadCatalog, loadDownloads, loadLoras, startPolling, state } from "./store";
import type { LoraPick, LoraStage } from "./store";
import { modelCards } from "./models";
import type { ModelCard } from "./models";
import type { DownloadPackage, Recipe } from "./types";

// Asks the app shell to switch pages (App.vue watches this).
export const navigation = reactive({ page: "" as "" | "create" | "gallery" | "models" | "scripts" });

// Busy flags per action key, for button spinners.
export const busy = reactive<Record<string, boolean>>({});

async function run<T>(key: string, action: () => Promise<T>): Promise<T | undefined> {
    busy[key] = true;
    try {
        return await action();
    } catch (error) {
        state.error = (error as Error).message;
        return undefined;
    } finally {
        busy[key] = false;
    }
}

export function startDownload(pkg: DownloadPackage) {
    return run(`download:${pkg.id}`, async () => {
        await api.startDownload(pkg.id);
        await loadDownloads();
        startPolling();
    });
}

export function stopDownload(pkg: DownloadPackage) {
    return run(`stop:${pkg.id}`, async () => {
        await api.cancelDownload(pkg.id);
        await loadDownloads();
    });
}

// Other packages and variants that use the same file, so deleting does not surprise anyone.
function sharedUse(pkg: DownloadPackage, path: string) {
    const users: string[] = [];
    for (const other of state.downloads?.packages || [])
        if (other.id !== pkg.id && other.components.some((c) => c.path === path && c.installed)) users.push(`model ${other.name}`);
    for (const recipe of state.recipes)
        if (recipe.source === "user" && Object.values(recipe.models || {}).includes(path)) users.push(`variant ${recipe.name}`);
    return users;
}

export async function deletePackageFiles(pkg: DownloadPackage) {
    const files = pkg.components.filter((c) => c.installed);
    if (!files.length) return;
    const total = files.reduce((sum, c) => sum + c.size, 0);
    const shared = files.flatMap((c) => sharedUse(pkg, c.path).map((user) => `${c.path.split("/").pop()} is also used by ${user}`));
    const ok = await confirmDanger(
        `Delete the files of ${pkg.name}?`,
        `${files.length} file(s), ${formatBytes(total)}, will be removed from disk. This cannot be undone; you can download them again.`,
        "Delete files",
        files.map((c) => `${c.path}  (${formatBytes(c.size)})`),
        shared.length ? `${shared.join(". ")}.` : undefined,
    );
    if (!ok) return;
    await run(`delete:${pkg.id}`, async () => {
        await api.deletePackageFiles(pkg.id);
        await Promise.all([loadDownloads(), loadCatalog()]);
    });
}

export async function removePackage(pkg: DownloadPackage) {
    const ok = pkg.overrides
        ? await confirmDanger("Undo your edits?", `Your copy of "${pkg.name}" is deleted and the ${pkg.overrides} version comes back. Downloaded files stay.`, "Undo edits")
        : await confirmDanger("Remove this model from the list?", `"${pkg.name}" is removed from the Models page. Downloaded files and variants stay.`, "Remove");
    if (!ok) return;
    await run(`remove:${pkg.id}`, async () => {
        await api.removeDownload(pkg.id);
        await loadDownloads();
    });
}

export async function deleteVariant(recipe: Recipe) {
    const ok = await confirmDanger("Delete this variant?", `"${recipe.name}" (${recipe.file}) is deleted. Model files are not touched.`, "Delete variant");
    if (!ok) return;
    await run(`variant:${recipe.id}`, async () => {
        await api.deleteRecipe(recipe.id);
        await loadCatalog();
    });
}

// Opens Create with a ready variant of this model selected.
// Opens Create with a downloaded LoRA added to a ready variant of a model it fits.
export async function useLora(card: ModelCard): Promise<string> {
    const lora = card.pkg?.lora;
    if (!lora) return "";
    await loadLoras();
    const fits = modelCards.value.filter((c) => c.pkg && lora.models?.includes(c.pkg.id)).flatMap((c) => c.readyVariants);
    const target = fits.find((r) => r.id === state.form.recipeId) || fits[0];
    if (!target) return `Download and set up ${lora.for} first; this LoRA only works with it.`;
    const picks: LoraPick[] = [];
    for (const stage of ["both", "high", "low"] as const) {
        const file = lora[stage];
        if (!file) continue;
        const name = file.split("/").pop() || file;
        const installed = state.loras.find((l) => l.path === name || l.path.endsWith(`/${name}`) || l.name === name.replace(/\.safetensors$/, ""));
        if (installed) picks.push({ path: installed.path, strength: lora.strength ?? 1, stage: stage as LoraStage });
    }
    if (!picks.length) return "The LoRA files were not found in the loras folder; try Rescan model folders in Settings.";
    const existing = (state.form.loras[target.id] || []).filter((p) => !picks.some((n) => n.path === p.path));
    state.form.loras = { ...state.form.loras, [target.id]: [...existing, ...picks] };
    state.form.recipeId = target.id;
    navigation.page = "create";
    return "";
}

export function useModel(card: ModelCard, variant?: Recipe) {
    const chosen = variant || card.readyVariants[0];
    if (!chosen) return;
    state.form.recipeId = chosen.id;
    navigation.page = "create";
}
