// Reads frames of a video (a File or a URL) in the browser as JPEG data URLs; the server takes
// videos as frame lists.

export interface FrameOptions {
    fps: number;
    // Output size. Omit both to keep the video's own size, rounded down to `multiple`.
    width?: number;
    height?: number;
    // Upper bound on frames; `native` reads at most the video's length at `fps`.
    frames: number;
    native?: boolean;
    multiple?: number;
    maxSide?: number;
    onProgress?: (done: number, total: number) => void;
}

export interface FrameClip {
    frames: string[];
    width: number;
    height: number;
    // The video is shorter than the requested clip; its last frame repeats.
    short: boolean;
}

function seek(video: HTMLVideoElement, time: number) {
    return new Promise<void>((resolve, reject) => {
        video.onseeked = () => resolve();
        video.onerror = () => reject(new Error("seek failed"));
        video.currentTime = time;
    });
}

export async function readVideoFrames(source: File | string, options: FrameOptions): Promise<FrameClip> {
    const url = typeof source === "string" ? source : URL.createObjectURL(source);
    const video = document.createElement("video");
    video.muted = true;
    video.crossOrigin = "anonymous";
    video.src = url;
    try {
        await new Promise<void>((resolve, reject) => {
            video.onloadeddata = () => resolve();
            video.onerror = () => reject(new Error("unsupported video"));
        });
        let width = options.width ?? video.videoWidth;
        let height = options.height ?? video.videoHeight;
        if (options.native) {
            const multiple = options.multiple || 16;
            const shrink = Math.min(1, (options.maxSide || Infinity) / Math.max(width, height));
            width = Math.max(multiple, Math.floor((width * shrink) / multiple) * multiple);
            height = Math.max(multiple, Math.floor((height * shrink) / multiple) * multiple);
        }
        const canvas = document.createElement("canvas");
        canvas.width = width;
        canvas.height = height;
        const context = canvas.getContext("2d")!;
        const available = Math.max(1, Math.floor(video.duration * options.fps + 1e-6));
        const count = Math.max(1, options.native ? Math.min(options.frames, available) : options.frames);
        const frames: string[] = [];
        for (let i = 0; i < count; i++) {
            options.onProgress?.(i, count);
            await seek(video, Math.max(0, Math.min(video.duration - 0.001, i / options.fps)));
            // Cover-crop to the output size so the frames line up with the result.
            const scale = Math.max(width / video.videoWidth, height / video.videoHeight);
            const w = video.videoWidth * scale;
            const h = video.videoHeight * scale;
            context.drawImage(video, (width - w) / 2, (height - h) / 2, w, h);
            frames.push(canvas.toDataURL("image/jpeg", 0.95));
        }
        return { frames, width, height, short: video.duration < (count - 1) / options.fps };
    } finally {
        if (typeof source !== "string") URL.revokeObjectURL(url);
    }
}
