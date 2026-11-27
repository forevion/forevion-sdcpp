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
    stage_ms?: Record<string, number>;
    generation_info?: unknown;
}

export interface DownloadComponent {
    path: string;
    size: number;
    installed: boolean;
    packed_from?: string;
}

export interface DownloadPackage {
    id: string;
    name: string;
    group?: string;
    description?: string;
    license?: string;
    homepage?: string;
    media?: string[];
    source: string;
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

export interface DownloadCatalog {
    packages: DownloadPackage[];
    errors: CatalogError[];
    ram_gb: number;
    model_root: string;
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
