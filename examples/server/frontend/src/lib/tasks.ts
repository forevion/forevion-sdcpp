import type { Recipe } from "./types";

// What the user wants to make. Each task needs certain inputs and is offered only when the
// chosen model declares the matching recipe feature.
export type TaskId = "t2i" | "i2i" | "edit" | "t2v" | "i2v" | "s2v" | "r2v" | "v2v";

export interface TaskDef {
    id: TaskId;
    label: string;
    hint: string;
    output: "image" | "video";
}

export const TASKS: TaskDef[] = [
    { id: "t2v", label: "Text → video", hint: "Describe a clip.", output: "video" },
    { id: "i2v", label: "Image → video", hint: "Animate a picture (optionally toward an end frame).", output: "video" },
    { id: "s2v", label: "Speech → video", hint: "Lip sync: a portrait talks along to an audio clip.", output: "video" },
    { id: "r2v", label: "References → video", hint: "Keep people, objects, voices from reference images and audio.", output: "video" },
    { id: "v2v", label: "Video → video", hint: "Follow the motion of a control video.", output: "video" },
    { id: "t2i", label: "Text → image", hint: "Describe a picture.", output: "image" },
    { id: "i2i", label: "Image → image", hint: "Redraw a picture; strength sets how much changes.", output: "image" },
    { id: "edit", label: "Edit image", hint: "Change pictures with an instruction, using them as references.", output: "image" },
];

export function isVideoRecipe(recipe: Recipe | undefined) {
    return Boolean(recipe && (recipe.video_constraints || recipe.defaults?.video_frames));
}

// Tasks a recipe supports, from its resolved features.
export function availableTasks(recipe: Recipe | undefined): TaskDef[] {
    if (!recipe) return [];
    const f = recipe.effective_features || {};
    const video = isVideoRecipe(recipe);
    const ids: TaskId[] = [];
    if (video) {
        ids.push("t2v");
        if (f.init_image) ids.push("i2v");
        if (f.ref_audios && f.init_image) ids.push("s2v");
        if (f.ref_images || (f.ref_audios && !f.init_image)) ids.push("r2v");
        if (f.control_frames) ids.push("v2v");
        ids.push("t2i");
    } else {
        ids.push("t2i");
        if (f.img2img) ids.push("i2i");
        if (f.ref_images) ids.push("edit");
    }
    return TASKS.filter((t) => ids.includes(t.id));
}

// Whether a recipe produces sound (it has an audio decoder input with a file chosen).
export function producesAudio(recipe: Recipe | undefined, files: Record<string, string>) {
    return Boolean(recipe?.slots.some((s) => s.id === "audio_vae") && files.audio_vae);
}

// Short capability badges for model cards.
export function capabilityBadges(recipe: Recipe | undefined, extra?: { features?: string[]; video?: boolean; audio?: boolean }) {
    const features = new Set(Object.entries(recipe?.effective_features || {}).filter(([, on]) => on).map(([key]) => key));
    for (const f of extra?.features || []) features.add(f);
    const video = extra?.video ?? isVideoRecipe(recipe);
    const badges: string[] = [];
    if (video) badges.push("Video");
    badges.push(video ? "Stills" : "Image");
    if (features.has("init_image") && video) badges.push("Image→video");
    if (features.has("end_image")) badges.push("Start/end frames");
    if (features.has("ref_audios")) badges.push("Lip sync");
    if (features.has("control_frames")) badges.push("Video→video");
    if (features.has("ref_images")) badges.push(video ? "References" : "Editing");
    if (!video && !features.has("ref_images") && recipe && recipe.effective_features?.img2img !== false) badges.push("Image→image");
    if (extra?.audio || recipe?.slots.some((s) => s.id === "audio_vae")) badges.push("Sound");
    return badges;
}
