const { defineConfig } = require("vite");
const vue = require("@vitejs/plugin-vue");
const { viteSingleFile } = require("vite-plugin-singlefile");

// `pnpm dev` proxies API calls to a running sd-server (SDCPP_SERVER, default http://127.0.0.1:1234).
const server = process.env.SDCPP_SERVER || "http://127.0.0.1:1234";

module.exports = defineConfig({
    plugins: [
        vue(),
        viteSingleFile(),
    ],
    server: {
        proxy: {
            "/sdcpp": server,
            "/v1": server,
            "/sdapi": server,
        },
    },
});
