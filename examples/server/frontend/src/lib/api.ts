import type {
    CatalogError,
    CatalogStatus,
    CustomModelInfo,
    CustomModelRequest,
    ModelSlotDef,
    PackageDefinition,
    StorageInfo,
    DglJob,
    DownloadCatalog,
    Job,
    JobSummaryEntry,
    ModelFile,
    OutputEntry,
    Recipe,
} from "./types";

async function fetchJson<T>(url: string, init?: RequestInit): Promise<T> {
    const response = await fetch(url, init);
    let payload: any = null;
    try {
        payload = await response.json();
    } catch {
        payload = null;
    }
    if (!response.ok) {
        throw new Error((payload && (payload.error || payload.message)) || `HTTP ${response.status}`);
    }
    return payload as T;
}

const post = (body?: unknown): RequestInit => ({
    method: "POST",
    headers: body === undefined ? undefined : { "Content-Type": "application/json" },
    body: body === undefined ? undefined : JSON.stringify(body),
});

export const api = {
    recipes: () => fetchJson<{ recipes: Recipe[]; errors: CatalogError[] }>("/sdcpp/v1/recipes"),
    files: () => fetchJson<{ files: ModelFile[]; errors: CatalogError[] }>("/sdcpp/v1/model-files"),
    refreshModels: () => fetchJson<{ errors: CatalogError[] }>("/sdcpp/v1/models/refresh", post()),
    submit: (kind: "img_gen" | "vid_gen", body: unknown) => fetchJson<Job>(`/sdcpp/v1/${kind}`, post(body)),
    job: (id: string) => fetchJson<Job>(`/sdcpp/v1/jobs/${id}`, { cache: "no-store" }),
    jobs: () => fetchJson<{ jobs: JobSummaryEntry[] }>("/sdcpp/v1/jobs", { cache: "no-store" }),
    cancelJob: (id: string) => fetchJson<Job>(`/sdcpp/v1/jobs/${id}/cancel`, post()),
    outputs: (limit = 500) => fetchJson<{ outputs: OutputEntry[]; total: number; enabled: boolean }>(`/sdcpp/v1/outputs?limit=${limit}`, { cache: "no-store" }),
    deleteOutput: (file: string) => fetchJson<{ deleted: boolean }>(`/sdcpp/v1/outputs/${file}`, { method: "DELETE" }),
    downloads: () => fetchJson<DownloadCatalog>("/sdcpp/v1/downloads", { cache: "no-store" }),
    startDownload: (id: string) => fetchJson<unknown>(`/sdcpp/v1/downloads/${id}`, post()),
    cancelDownload: (id: string) => fetchJson<unknown>(`/sdcpp/v1/downloads/${id}/cancel`, post()),
    removeDownload: (id: string) => fetchJson<unknown>(`/sdcpp/v1/downloads/${id}`, { method: "DELETE" }),
    deletePackageFiles: (id: string) => fetchJson<{ deleted: string[] }>(`/sdcpp/v1/downloads/${id}/files`, { method: "DELETE" }),
    packageDefinition: (id: string) => fetchJson<PackageDefinition>(`/sdcpp/v1/downloads/${id}/definition`, { cache: "no-store" }),
    savePackage: (pkg: Record<string, unknown>) => fetchJson<unknown>(`/sdcpp/v1/downloads/${pkg.id}`, { ...post(pkg), method: "PUT" }),
    refreshCatalog: () => fetchJson<CatalogStatus>("/sdcpp/v1/download-catalog/refresh", post()),
    addCustomModel: (body: CustomModelRequest) => fetchJson<CustomModelInfo>("/sdcpp/v1/download-catalog/custom", post(body)),
    modelSlots: () => fetchJson<{ slots: ModelSlotDef[]; categories: string[] }>("/sdcpp/v1/model-slots"),
    storage: () => fetchJson<StorageInfo>("/sdcpp/v1/storage", { cache: "no-store" }),
    deleteModelFile: (id: string) => fetchJson<unknown>(`/sdcpp/v1/model-files/${id.split("/").map(encodeURIComponent).join("/")}`, { method: "DELETE" }),
    saveRecipe: (recipe: Record<string, unknown>) =>
        fetchJson<{ saved: boolean; errors: CatalogError[] }>(`/sdcpp/v1/recipes/${recipe.id}`, { ...post(recipe), method: "PUT" }),
    deleteRecipe: (id: string) => fetchJson<unknown>(`/sdcpp/v1/recipes/${id}`, { method: "DELETE" }),
    capabilities: () => fetchJson<{ samplers: string[]; schedulers: string[] }>("/sdcpp/v1/capabilities"),
};

const dglPath = "/sdcpp/v1/dgl";

export function listDglScripts(): Promise<{ directory: string; scripts: string[] }> {
    return fetchJson(dglPath + "/scripts", { cache: "no-store" });
}

export function loadDglScript(name: string): Promise<{ source: string }> {
    return fetchJson(dglPath + "/script?" + new URLSearchParams({ name }), { cache: "no-store" });
}

export function saveDglScript(name: string, source: string, previousSource?: string): Promise<{ name: string }> {
    return fetchJson(dglPath + "/script", post({ name, source, previous_source: previousSource }));
}

export function submitDglJob(source: string, signal: AbortSignal): Promise<DglJob> {
    return fetchJson(dglPath + "/jobs", { ...post({ source }), signal });
}

export function getDglJob(id: string, signal: AbortSignal): Promise<DglJob> {
    return fetchJson(`${dglPath}/jobs/${encodeURIComponent(id)}`, { cache: "no-store", signal });
}

export function dglPreviewUrl(id: string, output: number, frame: number): string {
    return `${dglPath}/jobs/${encodeURIComponent(id)}/outputs/${output}?frame=${frame}`;
}
