<script setup lang="ts">
defineProps<{ name: string; size?: number }>();

const paths: Record<string, string> = {
    edit: "M4 20h4L19 9l-4-4L4 16zM13.5 6.5l4 4",
    create: "M12 3l1.9 4.6L18.5 9.5l-4.6 1.9L12 16l-1.9-4.6L5.5 9.5l4.6-1.9zM19 15l.9 2.1L22 18l-2.1.9L19 21l-.9-2.1L16 18l2.1-.9z",
    gallery: "M4 5h7v6H4zM13 5h7v9h-7zM4 13h7v6H4zM13 16h7v3h-7z",
    models: "M12 3l8 4.5v9L12 21l-8-4.5v-9zM12 3v18M4 7.5l8 4.5 8-4.5",
    scripts: "M8 7l-5 5 5 5M16 7l5 5-5 5M13.5 5l-3 14",
    queue: "M4 6h16M4 12h10M4 18h7M17 15l3 3-3 3",
    play: "M7 5l12 7-12 7z",
    stop: "M7 7h10v10H7z",
    close: "M6 6l12 12M18 6L6 18",
    download: "M12 4v11M7 10l5 5 5-5M5 20h14",
    trash: "M5 7h14M10 7V5h4v2M7 7l1 13h8l1-13",
    reuse: "M4 12a8 8 0 0 1 13.7-5.6L20 9M20 4v5h-5M20 12a8 8 0 0 1-13.7 5.6L4 15M4 20v-5h5",
    video: "M4 6h11v12H4zM15 10l5-3v10l-5-3",
    image: "M4 5h16v14H4zM8 10a1.5 1.5 0 1 0 0-.01M20 15l-5-5-9 9",
    check: "M5 12l5 5 9-10",
    chevron: "M9 6l6 6-6 6",
    dice: "M5 5h14v14H5zM9 9h.01M15 15h.01M15 9h.01M9 15h.01",
    lock: "M7 11V8a5 5 0 0 1 10 0v3M5 11h14v9H5z",
    refresh: "M20 12a8 8 0 1 1-2.3-5.7L20 9M20 4v5h-5",
    expand: "M4 9V4h5M20 9V4h-5M4 15v5h5M20 15v5h-5",
    sparkles: "M12 3v4M12 17v4M3 12h4M17 12h4M6 6l2.5 2.5M15.5 15.5L18 18M6 18l2.5-2.5M15.5 8.5L18 6",
};
</script>

<template>
    <svg
        :width="size ?? 18"
        :height="size ?? 18"
        viewBox="0 0 24 24"
        fill="none"
        stroke="currentColor"
        stroke-width="1.7"
        stroke-linecap="round"
        stroke-linejoin="round"
        aria-hidden="true"
    >
        <path :d="paths[name]" />
    </svg>
</template>
