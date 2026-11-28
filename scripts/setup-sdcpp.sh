#!/usr/bin/env bash
# Sets up stable-diffusion.cpp with the Studio Web UI on Linux (Debian/Ubuntu family,
# e.g. Zorin OS) and starts it. Run it again at any time: it checks what is installed,
# downloads or builds only when needed, and starts the server and the browser.
#
#   ./setup-sdcpp.sh                set up if needed, then start
#   ./setup-sdcpp.sh --update       get the latest release (or source), then start
#   ./setup-sdcpp.sh --from-source  build from source instead of downloading a release
#   ./setup-sdcpp.sh --gpu cuda     use an NVIDIA GPU (default: when nvidia-smi finds one)
#   ./setup-sdcpp.sh --help         all options
#
# By default it downloads the latest ready-built "studio-*" release from GitHub (x86-64,
# glibc 2.35+), checks its SHA-256 and unpacks it into app/ next to this script. Building
# from source needs Git, CMake, Ninja and a compiler from apt plus Node.js, which the script
# downloads next to itself. On the first start the browser asks where models, inputs and
# outputs go; the choice is saved in studio-settings.json next to this script.
set -euo pipefail

REPO_URL="${SDCPP_REPO:-https://github.com/forevion/forevion-sdcpp.git}"
BRANCH="${SDCPP_BRANCH:-develop}"
PACKAGE_NAME="sdcpp-studio-linux-x64"
GPU="${SDCPP_GPU:-auto}"
NODE_MAJOR=24
HOST=127.0.0.1
PORT=1234
OPEN_BROWSER=1
UPDATE=0
REBUILD=0
SETUP_ONLY=0
FROM_SOURCE=0
THREADS=""

usage() {
    cat <<EOF
Usage: $(basename "$0") [options]

  --update        install the latest release (or, when building from source, pull and
                  rebuild), then start
  --from-source   build from the $BRANCH branch instead of downloading a release
  --rebuild       rebuild from source even when the source has not changed
  --setup-only    install or build, but do not start the server
  --port N        server port (default $PORT)
  --lan           listen on all network interfaces so other devices can connect
                  (default: this computer only)
  --threads N     CPU threads (default: number of physical cores)
  --gpu MODE      auto, cuda or cpu (default auto: CUDA when nvidia-smi finds an
                  NVIDIA GPU). CUDA uses the ready-built CUDA package, or builds with
                  the CUDA toolkit (nvcc) when building from source
  --no-browser    do not open the browser
  -h, --help      show this help

Environment: SDCPP_REPO (default $REPO_URL), SDCPP_BRANCH (default $BRANCH),
SDCPP_GPU (same as --gpu), SDCPP_PACKAGE (install this package file or URL instead of the
latest release).
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --update) UPDATE=1 ;;
        --from-source) FROM_SOURCE=1 ;;
        --rebuild) REBUILD=1; FROM_SOURCE=1 ;;
        --setup-only|--build-only) SETUP_ONLY=1 ;;
        --port) PORT="${2:?--port needs a number}"; shift ;;
        --lan) HOST=0.0.0.0 ;;
        --threads) THREADS="${2:?--threads needs a number}"; shift ;;
        --gpu) GPU="${2:?--gpu needs auto, cuda or cpu}"; shift ;;
        --no-browser) OPEN_BROWSER=0 ;;
        -h|--help) usage; exit 0 ;;
        *) echo "Unknown option: $1" >&2; usage >&2; exit 2 ;;
    esac
    shift
done

if [[ -t 1 ]]; then
    BOLD=$'\e[1m'; DIM=$'\e[2m'; GREEN=$'\e[32m'; YELLOW=$'\e[33m'; RED=$'\e[31m'; RESET=$'\e[0m'
else
    BOLD=""; DIM=""; GREEN=""; YELLOW=""; RED=""; RESET=""
fi
step() { echo "${BOLD}==>${RESET} $*"; }
ok()   { echo "    ${GREEN}✓${RESET} $*"; }
warn() { echo "    ${YELLOW}!${RESET} $*"; }
die()  { echo "${RED}Error:${RESET} $*" >&2; exit 1; }

[[ "$(uname -s)" == "Linux" ]] || die "this script is for Linux. On Windows, use the release zip or docs/build.md."

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
if [[ -f "$SCRIPT_DIR/../examples/server/CMakeLists.txt" ]]; then
    # Run from inside a checkout: build it, keeping generated files in git-ignored folders.
    SRC="$(cd "$SCRIPT_DIR/.." && pwd)"
    HOME_DIR="$SRC"
    BUILD="$SRC/build-linux"
    TOOLS="$BUILD/tools"
    OUTPUTS="$SRC/output"
    FROM_SOURCE=1
    IN_CHECKOUT=1
else
    HOME_DIR="$SCRIPT_DIR"
    SRC="$HOME_DIR/stable-diffusion.cpp"
    BUILD="$HOME_DIR/build"
    TOOLS="$HOME_DIR/tools"
    OUTPUTS="$HOME_DIR/outputs"
    IN_CHECKOUT=0
fi
SETTINGS="$HOME_DIR/studio-settings.json"
APP="$HOME_DIR/app"
REPO_SLUG="$(sed -E 's#^https://github.com/##; s#\.git$##' <<<"$REPO_URL")"

# ================================================================ GPU

case "$GPU" in
    auto)
        if command -v nvidia-smi >/dev/null 2>&1 && nvidia-smi -L 2>/dev/null | grep -q '^GPU '; then
            GPU=cuda
        else
            GPU=cpu
        fi
        ;;
    cuda|cpu) ;;
    *) die "--gpu must be auto, cuda or cpu" ;;
esac
if [[ "$GPU" == cuda ]]; then
    command -v nvidia-smi >/dev/null 2>&1 && nvidia-smi -L >/dev/null 2>&1 ||
        die "--gpu cuda needs an NVIDIA GPU with its driver (nvidia-smi does not work); use --gpu cpu"
    PACKAGE_NAME="sdcpp-studio-linux-x64-cuda"
    BUILD="$BUILD-cuda"
fi

if [[ -x "$SCRIPT_DIR/sd-server" && $FROM_SOURCE -eq 0 ]]; then
    # Run from an unpacked release archive: use the binaries next to this script.
    APP="$SCRIPT_DIR"
    BUNDLED=1
    # An unpacked archive runs what it contains, whatever --gpu says.
    if [[ "$(cat "$SCRIPT_DIR/PACKAGE" 2>/dev/null)" == *-cuda ]]; then GPU=cuda; else GPU=cpu; fi
else
    BUNDLED=0
fi
if [[ $FROM_SOURCE -eq 0 && $BUNDLED -eq 0 && "$(uname -m)" != "x86_64" ]]; then
    warn "no ready-built package for $(uname -m); building from source"
    FROM_SOURCE=1
fi

# Installs Debian/Ubuntu packages for the commands (or libraries) that are missing.
apt_install() {
    [[ $# -gt 0 ]] || return 0
    command -v apt-get >/dev/null 2>&1 ||
        die "missing: $*. Install them with your package manager and run this script again."
    # Cloud GPU containers (RunPod, Vast.ai, ...) run as root and often have no sudo.
    local -a as_root=()
    if [[ $EUID -ne 0 ]]; then
        command -v sudo >/dev/null 2>&1 || die "missing: $*. Install them as root (apt-get install $*) and run this script again."
        as_root=(sudo)
        warn "installing $* (asks for your password)"
    else
        warn "installing $*"
    fi
    "${as_root[@]}" apt-get update
    "${as_root[@]}" env DEBIAN_FRONTEND=noninteractive apt-get install -y "$@" ca-certificates
}

require_commands() {
    local -a missing=()
    local entry
    for entry in "$@"; do
        command -v "${entry%%=*}" >/dev/null 2>&1 || missing+=("${entry#*=}")
    done
    if [[ ${#missing[@]} -gt 0 ]]; then
        mapfile -t missing < <(printf '%s\n' "${missing[@]}" | sort -u)
        apt_install "${missing[@]}"
    fi
}

# ================================================================ ready-built release

latest_release_tag() {
    curl -fsSL -m 30 -H "Accept: application/vnd.github+json" \
        "https://api.github.com/repos/$REPO_SLUG/releases?per_page=30" 2>/dev/null |
        grep -oE '"tag_name": *"studio-[^"]+"' | head -1 | sed -E 's/.*"(studio-[^"]+)"$/\1/' || true
}

install_release() {
    step "Checking the Forevion Studio package"
    require_commands curl=curl tar=tar gzip=gzip sha256sum=coreutils

    local installed tag work package
    installed="$(cat "$APP/RELEASE" 2>/dev/null || true)"
    if [[ $BUNDLED -eq 1 ]]; then
        ok "using the package in $APP ($(cat "$APP/VERSION" 2>/dev/null || echo unknown version))"
        [[ $UPDATE -eq 0 ]] || warn "--update is not available inside an unpacked archive; download the new release instead"
        return 0
    fi
    if [[ $BUNDLED -eq 0 && -x "$APP/sd-server" && "$(cat "$APP/PACKAGE" 2>/dev/null || echo sdcpp-studio-linux-x64)" != "$PACKAGE_NAME" ]]; then
        warn "the installed package is not $PACKAGE_NAME; replacing it"
        installed=""
    elif [[ -x "$APP/sd-server" && $UPDATE -eq 0 && -z "${SDCPP_PACKAGE:-}" ]]; then
        ok "installed: $installed"
        return 0
    fi

    work="$HOME_DIR/.download"
    rm -rf "$work" && mkdir -p "$work"
    if [[ -n "${SDCPP_PACKAGE:-}" ]]; then
        tag="local:$(basename "$SDCPP_PACKAGE")"
        if [[ -f "$SDCPP_PACKAGE" ]]; then
            cp "$SDCPP_PACKAGE" "$work/$PACKAGE_NAME.tar.gz"
        else
            curl -fL --progress-bar -o "$work/$PACKAGE_NAME.tar.gz" "$SDCPP_PACKAGE"
        fi
        warn "installing $SDCPP_PACKAGE without a checksum"
    else
        tag="$(latest_release_tag)"
        if [[ -z "$tag" ]]; then
            rm -rf "$work"
            if [[ -x "$APP/sd-server" ]]; then
                warn "cannot reach the release list; keeping $installed"
                return 0
            fi
            warn "no ready-built release found on github.com/$REPO_SLUG; building from source instead"
            FROM_SOURCE=1
            return 0
        fi
        if ! curl -fsIL -o /dev/null "https://github.com/$REPO_SLUG/releases/download/$tag/$PACKAGE_NAME.tar.gz"; then
            rm -rf "$work"
            if [[ "$GPU" == cuda ]] && { command -v nvcc >/dev/null 2>&1 || [[ -x /usr/local/cuda/bin/nvcc ]]; }; then
                warn "release $tag has no $PACKAGE_NAME package; building with the CUDA toolkit instead"
                FROM_SOURCE=1
                return 0
            fi
            die "release $tag has no $PACKAGE_NAME package. Install the CUDA toolkit and use --from-source, or use --gpu cpu."
        fi
        if [[ "$tag" == "$installed" && -x "$APP/sd-server" ]]; then
            rm -rf "$work"
            ok "already the latest release ($tag)"
            return 0
        fi
        local base="https://github.com/$REPO_SLUG/releases/download/$tag"
        warn "downloading $tag"
        curl -fL --progress-bar -o "$work/$PACKAGE_NAME.tar.gz" "$base/$PACKAGE_NAME.tar.gz"
        curl -fsSL -o "$work/SHA256SUMS.txt" "$base/SHA256SUMS.txt"
        (cd "$work" && grep " $PACKAGE_NAME.tar.gz\$" SHA256SUMS.txt | sha256sum -c --quiet -) ||
            die "the download failed its SHA-256 check; run the script again"
        ok "checksum verified"
    fi

    mkdir -p "$work/unpacked"
    tar -xzf "$work/$PACKAGE_NAME.tar.gz" -C "$work/unpacked" --strip-components=1
    [[ -x "$work/unpacked/sd-server" ]] || die "the package does not contain sd-server"
    echo "$tag" >"$work/unpacked/RELEASE"
    echo "$PACKAGE_NAME" >"$work/unpacked/PACKAGE"
    rm -rf "$APP.old"
    [[ -d "$APP" ]] && mv "$APP" "$APP.old"
    mv "$work/unpacked" "$APP"
    rm -rf "$APP.old" "$work"
    ok "installed $tag into $APP"
}

# The package only needs the C/C++ runtime and OpenMP; minimal systems may lack libgomp.
check_runtime_libraries() {
    local missing
    missing="$( (ldd "$APP/sd-server" "$APP"/lib*.so* 2>/dev/null || true) | awk '/not found/ {print $1}' | sort -u)"
    [[ -n "$missing" ]] || return 0
    local -a packages=()
    while read -r lib; do
        case "$lib" in
            libgomp.so*) packages+=(libgomp1) ;;
            libstdc++.so*) packages+=(libstdc++6) ;;
            libgcc_s.so*) packages+=(libgcc-s1) ;;
            libcuda.so*) die "the NVIDIA driver library ($lib) is missing; install the NVIDIA driver or use --gpu cpu" ;;
            *) die "a required library is missing: $lib" ;;
        esac
    done <<<"$missing"
    apt_install "${packages[@]}"
}

# ================================================================ build from source

build_from_source() {
    step "Checking build tools"
    require_commands git=git cmake=cmake ninja=ninja-build g++=build-essential make=build-essential \
        curl=curl xz=xz-utils sha256sum=coreutils
    local cmake_version
    cmake_version="$(cmake --version | head -1 | grep -oE '[0-9]+\.[0-9]+' | head -1)"
    if [[ "$(printf '%s\n3.14\n' "$cmake_version" | sort -V | head -1)" != "3.14" ]]; then
        die "CMake $cmake_version is too old (3.14 or newer needed)."
    fi
    ok "git, cmake $cmake_version, ninja, $(g++ -dumpfullversion 2>/dev/null | sed 's/^/g++ /'), curl"

    mkdir -p "$TOOLS"
    export npm_config_cache="$TOOLS/npm-cache"
    export npm_config_store_dir="$TOOLS/pnpm-store"
    step "Checking Node.js (builds the Web UI)"
    [[ -x "$TOOLS/node/bin/node" ]] && export PATH="$TOOLS/node/bin:$PATH"
    if ! { command -v node >/dev/null 2>&1 && [[ "$(node -p 'process.versions.node.split(".")[0]')" -ge 20 ]]; }; then
        local node_arch base sums file
        case "$(uname -m)" in
            x86_64) node_arch=x64 ;;
            aarch64|arm64) node_arch=arm64 ;;
            *) die "no Node.js build for $(uname -m); install Node.js 20 or newer and run again." ;;
        esac
        base="https://nodejs.org/dist/latest-v${NODE_MAJOR}.x"
        sums="$(curl -fsSL "$base/SHASUMS256.txt")" || die "cannot reach nodejs.org"
        file="$(grep -oE "node-v[0-9.]+-linux-${node_arch}\.tar\.xz" <<<"$sums" | head -1)"
        [[ -n "$file" ]] || die "no Node.js $NODE_MAJOR download found for linux-$node_arch"
        warn "downloading $file into $TOOLS"
        curl -fL --progress-bar -o "$TOOLS/$file" "$base/$file"
        (cd "$TOOLS" && grep " $file\$" <<<"$sums" | sha256sum -c --quiet -) || die "Node.js download failed its checksum"
        rm -rf "$TOOLS/node" && mkdir -p "$TOOLS/node"
        tar -xJf "$TOOLS/$file" -C "$TOOLS/node" --strip-components=1
        rm -f "$TOOLS/$file"
        export PATH="$TOOLS/node/bin:$PATH"
    fi
    ok "node $(node --version)"

    local pnpm_version="10.15.1"
    if [[ -f "$SRC/examples/server/frontend/package.json" ]]; then
        pnpm_version="$(grep -oE '"packageManager": *"pnpm@[0-9.]+' "$SRC/examples/server/frontend/package.json" | grep -oE '[0-9.]+$' || echo "$pnpm_version")"
    fi
    if [[ ! -x "$TOOLS/pnpm/node_modules/.bin/pnpm" ]] || [[ "$("$TOOLS/pnpm/node_modules/.bin/pnpm" --version)" != "$pnpm_version" ]]; then
        warn "installing pnpm $pnpm_version into $TOOLS/pnpm"
        npm install --silent --no-audit --no-fund --prefix "$TOOLS/pnpm" "pnpm@$pnpm_version" >/dev/null
    fi
    export PATH="$TOOLS/pnpm/node_modules/.bin:$PATH"
    ok "pnpm $(pnpm --version)"

    step "Checking the source"
    local -a submodules=(ggml thirdparty/libwebp thirdparty/libwebm) missing_submodules=()
    [[ "$GPU" == cuda ]] && submodules+=(thirdparty/cutlass)
    if [[ ! -d "$SRC/.git" ]]; then
        warn "cloning $REPO_URL ($BRANCH)"
        git clone --branch "$BRANCH" "$REPO_URL" "$SRC"
        git -C "$SRC" submodule update --init --depth 1 "${submodules[@]}"
    elif [[ $UPDATE -eq 1 ]]; then
        if [[ -n "$(git -C "$SRC" status --porcelain --untracked-files=no)" ]]; then
            die "$SRC has local changes; commit or stash them before --update."
        fi
        git -C "$SRC" pull --ff-only
        git -C "$SRC" submodule update --init --depth 1 "${submodules[@]}"
    fi
    local module
    for module in "${submodules[@]}"; do
        [[ -e "$SRC/$module/CMakeLists.txt" ]] || missing_submodules+=("$module")
    done
    if [[ ${#missing_submodules[@]} -gt 0 ]]; then
        git -C "$SRC" submodule update --init --depth 1 "${missing_submodules[@]}"
    fi
    local commit stamp="$BUILD/.built-commit"
    commit="$(git -C "$SRC" rev-parse --short HEAD)"
    ok "$(git -C "$SRC" rev-parse --abbrev-ref HEAD) at $commit"

    local needs_build=0
    if [[ ! -x "$BUILD/bin/sd-server" ]] || [[ $REBUILD -eq 1 ]] || [[ "$(cat "$stamp" 2>/dev/null || true)" != "$commit" ]]; then
        needs_build=1
    elif [[ $IN_CHECKOUT -eq 1 ]] && [[ -n "$(git -C "$SRC" status --porcelain --untracked-files=no)" ]]; then
        needs_build=1
    fi
    local -a backend=(-DGGML_NATIVE=ON) label="CPU, tuned for this processor"
    if [[ "$GPU" == cuda ]]; then
        local nvcc
        nvcc="$(command -v nvcc || true)"
        [[ -n "$nvcc" || ! -x /usr/local/cuda/bin/nvcc ]] || nvcc=/usr/local/cuda/bin/nvcc
        [[ -n "$nvcc" ]] || die "building for CUDA needs the CUDA toolkit (nvcc, 12.x). Install it from developer.nvidia.com/cuda-downloads, or use the ready-built CUDA package (run without --from-source)."
        ok "$("$nvcc" --version | grep -oE 'release [0-9.]+' | sed 's/release/CUDA/')"
        backend+=(-DSD_CUDA=ON -DCMAKE_CUDA_COMPILER="$nvcc" -DCMAKE_CUDA_ARCHITECTURES=native)
        label="CUDA for this GPU and CPU; the first build takes 15-40 minutes"
    fi
    if [[ $needs_build -eq 1 ]]; then
        step "Building sd.cpp ($label)"
        mkdir -p "$BUILD"
        local log="$BUILD/build.log" mem_gb jobs started
        # Large translation units need up to ~2 GB each; one job per CPU thread can exhaust RAM.
        mem_gb="$(awk '/MemAvailable/ {printf "%d", $2 / 1048576}' /proc/meminfo)"
        jobs=$(( mem_gb / 2 ))
        (( jobs > $(nproc) )) && jobs=$(nproc)
        (( jobs < 1 )) && jobs=1
        echo "    ${DIM}$jobs parallel compile jobs (${mem_gb} GB RAM available); log: $log${RESET}"
        started=$SECONDS
        if ! {
            cmake -S "$SRC" -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release \
                "${backend[@]}" -DSD_SERVER_BUILD_FRONTEND=ON &&
            cmake --build "$BUILD" --target sd-server sd-cli -j "$jobs"
        } >"$log" 2>&1; then
            tail -n 40 "$log" >&2
            die "the build failed; the full log is $log"
        fi
        echo "$commit" >"$stamp"
        ok "built in $((SECONDS - started)) s"
    else
        ok "build is up to date ($commit)"
    fi
    APP="$BUILD/bin"
}

if [[ $FROM_SOURCE -eq 0 ]]; then
    install_release
fi
if [[ $FROM_SOURCE -eq 1 ]]; then
    build_from_source
else
    check_runtime_libraries
fi
SERVER_BIN="$APP/sd-server"

if [[ $SETUP_ONLY -eq 1 ]]; then
    ok "ready: $SERVER_BIN"
    exit 0
fi

# ================================================================ start

# A folder saved by the Web UI in studio-settings.json, e.g. settings_folder models.
settings_folder() {
    [[ -f "$SETTINGS" ]] || return 0
    sed -n "s/.*\"$1_dir\": *\"\([^\"]*\)\".*/\1/p" "$SETTINGS" | head -n 1
}
MODELS="$(settings_folder models)"
OUTPUTS="$(settings_folder outputs)"
if [[ -z "$THREADS" ]]; then
    THREADS="$( (lscpu -p=Core,Socket 2>/dev/null || true) | grep -v '^#' | sort -u | wc -l)"
    [[ "$THREADS" -gt 0 ]] || THREADS="$(nproc)"
fi
URL="http://127.0.0.1:$PORT"
[[ "$HOST" == "0.0.0.0" ]] && LAN_URL="http://$( (hostname -I 2>/dev/null || true) | awk '{print $1}'):$PORT" || LAN_URL=""

open_browser() {
    [[ $OPEN_BROWSER -eq 1 ]] || return 0
    command -v xdg-open >/dev/null 2>&1 && [[ -n "${DISPLAY:-}${WAYLAND_DISPLAY:-}" ]] || return 0
    xdg-open "$URL" >/dev/null 2>&1 &
}

if curl -fsS -m 2 "$URL/sdcpp/v1/capabilities" >/dev/null 2>&1; then
    ok "sd.cpp is already running at $URL"
    open_browser
    exit 0
fi

ram_gb="$(awk '/MemTotal/ {printf "%d", $2 / 1048576}' /proc/meminfo)"
free_gb="$(df -Pk "${MODELS:-$HOME_DIR}" 2>/dev/null | awk 'NR == 2 {printf "%d", $4 / 1048576}')"
free_gb="${free_gb:-0}"
step "Starting Forevion Studio"
if [[ -n "$MODELS" ]]; then
    echo "    ${DIM}models:  $MODELS${RESET}"
    echo "    ${DIM}outputs: $OUTPUTS${RESET}"
else
    echo "    ${DIM}folders: not chosen yet; the browser asks for them first${RESET}"
fi
echo "    ${DIM}$THREADS threads, ${ram_gb} GB RAM, ${free_gb} GB free disk${RESET}"
if [[ "$GPU" == cuda ]]; then
    nvidia-smi --query-gpu=name,memory.total --format=csv,noheader 2>/dev/null | sed "s/^/    ${DIM}GPU: /; s/\$/${RESET}/"
fi
[[ "$free_gb" -ge 30 ]] || warn "less than 30 GB free; the models need 11-23 GB each"
echo
echo "    Open ${BOLD}$URL${RESET}${LAN_URL:+  (other devices: $LAN_URL)}"
echo "    Get models on the Models page, then make images and videos on Create."
echo "    Press Ctrl+C here to stop."
echo

(
    for _ in $(seq 1 120); do
        if curl -fsS -m 2 "$URL/sdcpp/v1/capabilities" >/dev/null 2>&1; then
            open_browser
            exit 0
        fi
        sleep 1
    done
) &

# CPU: keep every weight in RAM. GPU: --auto-fit (on by default) puts weights in VRAM first.
GPU_ARGS=()
[[ "$GPU" == cuda ]] || GPU_ARGS=(--auto-fit off)

cd "$HOME_DIR"
exec "$SERVER_BIN" \
    --listen-ip "$HOST" --listen-port "$PORT" \
    --settings-file "$SETTINGS" \
    --mmap --vae-tiling -t "$THREADS" "${GPU_ARGS[@]}"
