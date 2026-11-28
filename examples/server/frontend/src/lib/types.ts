export interface CatalogError {
    file: string;
    message: string;
}

export interface VideoConstraints {
    frame_step?: number;
    frame_offset?: number;
    frame_rounding?: "floor" | "ceil";
    min_frames?: number;
    max_frames?: number;
    fixed_fps?: number;
}

export interface RecipeDefaults {
    width?: number;
    height?: number;
    steps?: number;
    cfg_scale?: number;
    sampling_method?: string;
    scheduler?: string;
    video_frames?: number;
    fps?: number;
    flow_shift?: number;
    custom_sigmas?: number[];
}

export interface RecipeSlot {
    id: string;
    label: string;
    category: string;
    categories?: string[];
    required: boolean;
}

export interface Recipe {
    id: string;
    name: string;
    description?: string;
    model_url?: string;
    size_multiple?: number;
    video_constraints?: VideoConstraints;
    required_any?: string[][];
    source: string;
    file?: string;
    models?: Record<string, string>;
    defaults?: RecipeDefaults;
    effective_features?: Record<string, boolean>;
    slots: RecipeSlot[];
}

export interface ModelFile {
    id: string;
    name?: string;
    category: string;
}

export interface JobProgress {
    stage: string;
    detail: string;
    pass: number;
    completed: number | null;
    total: number | null;
    unit: string | null;
    stage_elapsed_ms: number;
    elapsed_ms: number;
}

export interface JobSummary {
    kind: "img_gen" | "vid_gen";
    prompt: string;
    negative_prompt?: string;
    width: number | null;
    height: number | null;
    seed: number | null;
    steps: number | null;
    video_frames?: number | null;
    fps?: number | null;
    recipe?: string;
    models?: Record<string, string>;
    // Where the job ran, for example "GPU · NVIDIA L4" or "CPU".
    device?: string;
}

export interface Job {
    id: string;
    kind?: string;
    status: "queued" | "generating" | "completed" | "failed" | "cancelled";
    cancel_requested?: boolean;
    queue_position?: number;
    created?: number;
    started?: number | null;
    completed?: number | null;
    progress?: JobProgress | null;
    preview?: { pass: number; step: number; total_steps: number; b64_json: string } | null;
    result?: any;
    error?: { code: string; message: string } | null;
}

export interface JobSummaryEntry extends Job {
    summary: JobSummary | null;
}

export interface OutputEntry extends JobSummary {
    file: string;
    url: string;
    mime_type: string;
    bytes: number;
    job_id: string;
    created: number;
    started: number;
    completed: number;
    elapsed_ms: number;
    frame_count?: number;
    rendered_frames?: number | null;
    preview_url?: string;
    has_audio?: boolean;
    // Uploaded inputs kept in the inputs folder, as URLs by request field.
    inputs?: { init_image?: string; end_image?: string; ref_images?: string[]; ref_audios?: string[]; control_frames?: string[] };
    stage_ms?: Record<string, number>;
    generation_info?: unknown;
}

export interface DownloadComponent {
    path: string;
    size: number;
    installed: boolean;
    packed_from?: string;
}

export interface ModelGuide {
    notes?: string[];
    inputs?: Record<string, string>;
    alternatives?: Record<string, string>;
    settings?: {
        size_multiple?: number;
        video_constraints?: VideoConstraints;
        defaults?: RecipeDefaults;
        features?: string[];
        disabled_features?: string[];
        model_url?: string;
    };
}

export interface DownloadPackage {
    guide?: ModelGuide;
    id: string;
    name: string;
    group?: string;
    description?: string;
    license?: string;
    homepage?: string;
    media?: string[];
    source: string;
    overrides?: "builtin" | "online";
    installed: boolean;
    size: number;
    missing_bytes: number;
    components: DownloadComponent[];
    status: {
        state: "idle" | "queued" | "starting" | "downloading" | "verifying" | "packing" | "completed" | "failed" | "cancelled";
        detail: string;
        error: string | null;
        done_bytes: number;
        total_bytes: number;
        speed_bps: number;
        queue_position: number;
    };
}

export interface CatalogStatus {
    state: "checking" | "ok" | "error" | "disabled";
    url?: string;
    error?: string;
    checked?: number;
}

export interface DownloadCatalog {
    packages: DownloadPackage[];
    errors: CatalogError[];
    ram_gb: number;
    model_root: string;
    catalog: CatalogStatus;
}

export interface PackageFile {
    path: string;
    url: string;
    size?: number;
    sha256?: string;
}

export interface PackageDefinition {
    package: {
        id: string;
        name: string;
        group?: string;
        description?: string;
        license?: string;
        homepage?: string;
        media?: string[];
        files?: PackageFile[];
        convert?: unknown[];
        recipes?: unknown[];
        [key: string]: unknown;
    };
    source: string;
    overrides: "builtin" | "online" | null;
    editable: boolean;
}

export interface CustomModelRequest {
    url: string;
    category: string;
    filename?: string;
    name?: string;
    dry_run?: boolean;
    start?: boolean;
}

export interface CustomModelInfo {
    id?: string;
    url: string;
    path: string;
    size: number;
    sha256: string;
    homepage: string;
    exists: boolean;
}

export interface ModelSlotDef {
    id: string;
    label: string;
    categories: string[];
}

export interface StorageFile {
    id: string;
    name: string;
    category: string;
    size: number;
    modified: number;
    used_by: { id: string; name: string }[];
    loaded: boolean;
}

export interface StorageInfo {
    files: StorageFile[];
    roots: { id: string; path: string; free_bytes: number; total_bytes: number }[];
}

export interface DglOutput {
    name: string;
    dtype: number;
    shape: number[];
    bytes: number;
    frames: number;
    value?: string | number | null;
}

export interface DglMedia {
    name: string;
    mime_type: "image/png" | "video/webm" | "video/x-msvideo";
    b64_json?: string;
    error?: { code: string; message: string };
}

export interface DglJob {
    id: string;
    status: "queued" | "generating" | "completed" | "failed" | "cancelled";
    queue_position: number;
    result: { type: string; outputs: DglOutput[]; media: DglMedia[] } | null;
    error: { code: string; message: string } | null;
}

export interface ComputeDevice {
    name: string;
    description: string;
    type: "gpu" | "igpu" | "accel" | "cpu";
    memory_total: number;
    memory_free: number;
}

export interface SystemInfo {
    devices: ComputeDevice[];
    // GPU backend shipped with this build ("cuda", "vulkan", ...), empty for CPU-only builds.
    gpu_build: string;
    gpu_found: boolean;
    auto_fit: boolean;
    placement: { kind: "gpu" | "cpu" | "mixed"; label: string; components: { name: string; compute: string; params: string; params_mib: number }[] };
}

export type FolderKey = "models" | "inputs" | "outputs";

export interface FolderSettings {
    settings_file: string;
    can_save: boolean;
    needs_setup: boolean;
    can_browse: boolean;
    folders: Record<FolderKey, { path: string; exists: boolean }>;
    suggested: Record<FolderKey, string>;
}

export interface FolderListing {
    path: string;
    parent: string;
    dirs: { name: string; path: string }[];
    roots: { name: string; path: string }[];
    warning: string;
}
