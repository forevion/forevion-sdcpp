#!/usr/bin/env bash
# Sets up stable-diffusion.cpp with the Studio Web UI on Linux (Debian/Ubuntu family,
# e.g. Zorin OS) and starts it. Run it again at any time: it checks the tools, builds only
# when needed, and starts the server and the browser.
#
#   ./setup-sdcpp.sh              set up if needed, then start
#   ./setup-sdcpp.sh --update     pull the latest source, rebuild, then start
#   ./setup-sdcpp.sh --help       all options
#
# Everything goes next to this script (source, build, Node.js, models, outputs). Only the
# compiler, CMake, Ninja, Git and curl come from apt, and only when they are missing.
set -euo pipefail

REPO_URL="${SDCPP_REPO:-https://github.com/forevion/forevion-sdcpp.git}"
BRANCH="${SDCPP_BRANCH:-develop}"
NODE_MAJOR=24
HOST=127.0.0.1
PORT=1234
OPEN_BROWSER=1
UPDATE=0
REBUILD=0
BUILD_ONLY=0
THREADS=""

usage() {
    cat <<EOF
Usage: $(basename "$0") [options]

  --update        pull the latest $BRANCH branch, rebuild, then start
  --rebuild       rebuild even when the source has not changed
  --build-only    set up and build, but do not start the server
  --port N        server port (default $PORT)
  --lan           listen on all network interfaces so other devices can connect
                  (default: this computer only)
  --threads N     CPU threads (default: number of physical cores)
  --no-browser    do not open the browser
  -h, --help      show this help

Environment: SDCPP_REPO (default $REPO_URL), SDCPP_BRANCH (default $BRANCH).
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --update) UPDATE=1 ;;
        --rebuild) REBUILD=1 ;;
        --build-only) BUILD_ONLY=1 ;;
        --port) PORT="${2:?--port needs a number}"; shift ;;
        --lan) HOST=0.0.0.0 ;;
        --threads) THREADS="${2:?--threads needs a number}"; shift ;;
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

[[ "$(uname -s)" == "Linux" ]] || die "this script is for Linux. On Windows, build with the steps in docs/build.md."

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
if [[ -f "$SCRIPT_DIR/../examples/server/CMakeLists.txt" ]]; then
    # Run from inside a checkout: use it, and keep generated files in git-ignored folders.
    SRC="$(cd "$SCRIPT_DIR/.." && pwd)"
    HOME_DIR="$SRC"
    BUILD="$SRC/build-linux"
    TOOLS="$BUILD/tools"
    OUTPUTS="$SRC/output"
    IN_CHECKOUT=1
else
    HOME_DIR="$SCRIPT_DIR"
    SRC="$HOME_DIR/stable-diffusion.cpp"
    BUILD="$HOME_DIR/build"
    TOOLS="$HOME_DIR/tools"
    OUTPUTS="$HOME_DIR/outputs"
    IN_CHECKOUT=0
fi
MODELS="$HOME_DIR/models"
SERVER_BIN="$BUILD/bin/sd-server"
STAMP="$BUILD/.built-commit"

# ---------------------------------------------------------------- system packages

step "Checking build tools"
declare -A PKG_FOR=([git]=git [cmake]=cmake [ninja]=ninja-build [g++]=build-essential [make]=build-essential
                    [curl]=curl [xz]=xz-utils [sha256sum]=coreutils)
missing=()
for tool in git cmake ninja g++ make curl xz sha256sum; do
    command -v "$tool" >/dev/null 2>&1 || missing+=("${PKG_FOR[$tool]}")
done
if [[ ${#missing[@]} -gt 0 ]]; then
    mapfile -t missing < <(printf '%s\n' "${missing[@]}" | sort -u)
    command -v apt-get >/dev/null 2>&1 ||
        die "missing: ${missing[*]}. Install them with your package manager and run this script again."
    warn "installing ${missing[*]} (asks for your password)"
    sudo apt-get update
    sudo apt-get install -y "${missing[@]}" ca-certificates
fi
cmake_version="$(cmake --version | head -1 | grep -oE '[0-9]+\.[0-9]+' | head -1)"
if [[ "$(printf '%s\n3.14\n' "$cmake_version" | sort -V | head -1)" != "3.14" ]]; then
    die "CMake $cmake_version is too old (3.14 or newer needed)."
fi
ok "git, cmake $cmake_version, ninja, $(g++ -dumpfullversion 2>/dev/null | sed 's/^/g++ /'), curl"

# ---------------------------------------------------------------- Node.js + pnpm (local)

mkdir -p "$TOOLS"
export npm_config_cache="$TOOLS/npm-cache"
export npm_config_store_dir="$TOOLS/pnpm-store"

node_ok() { command -v node >/dev/null 2>&1 && [[ "$(node -p 'process.versions.node.split(".")[0]')" -ge 20 ]]; }

step "Checking Node.js (builds the Web UI)"
if [[ -x "$TOOLS/node/bin/node" ]]; then
    export PATH="$TOOLS/node/bin:$PATH"
fi
if ! node_ok; then
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

PNPM_VERSION="10.15.1"
if [[ -f "$SRC/examples/server/frontend/package.json" ]]; then
    PNPM_VERSION="$(grep -oE '"packageManager": *"pnpm@[0-9.]+' "$SRC/examples/server/frontend/package.json" | grep -oE '[0-9.]+$' || echo "$PNPM_VERSION")"
fi
if [[ ! -x "$TOOLS/pnpm/node_modules/.bin/pnpm" ]] || [[ "$("$TOOLS/pnpm/node_modules/.bin/pnpm" --version)" != "$PNPM_VERSION" ]]; then
    warn "installing pnpm $PNPM_VERSION into $TOOLS/pnpm"
    npm install --silent --no-audit --no-fund --prefix "$TOOLS/pnpm" "pnpm@$PNPM_VERSION" >/dev/null
fi
export PATH="$TOOLS/pnpm/node_modules/.bin:$PATH"
ok "pnpm $(pnpm --version)"

# ---------------------------------------------------------------- source

step "Checking the source"
submodules=(ggml thirdparty/libwebp thirdparty/libwebm)
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
missing_submodules=()
for module in "${submodules[@]}"; do
    [[ -e "$SRC/$module/CMakeLists.txt" ]] || missing_submodules+=("$module")
done
if [[ ${#missing_submodules[@]} -gt 0 ]]; then
    git -C "$SRC" submodule update --init --depth 1 "${missing_submodules[@]}"
fi
COMMIT="$(git -C "$SRC" rev-parse --short HEAD)"
ok "$(git -C "$SRC" rev-parse --abbrev-ref HEAD) at $COMMIT"

# ---------------------------------------------------------------- build

needs_build=0
if [[ ! -x "$SERVER_BIN" ]]; then
    needs_build=1
elif [[ $REBUILD -eq 1 ]] || [[ "$(cat "$STAMP" 2>/dev/null || true)" != "$COMMIT" ]]; then
    needs_build=1
elif [[ $IN_CHECKOUT -eq 1 ]] && [[ -n "$(git -C "$SRC" status --porcelain --untracked-files=no)" ]]; then
    needs_build=1
fi

if [[ $needs_build -eq 1 ]]; then
    step "Building sd.cpp (CPU, tuned for this processor; the first build takes several minutes)"
    mkdir -p "$BUILD"
    log="$BUILD/build.log"
    # Large translation units need up to ~2 GB each; one job per CPU thread can exhaust RAM.
    mem_gb="$(awk '/MemAvailable/ {printf "%d", $2 / 1048576}' /proc/meminfo)"
    jobs=$(( mem_gb / 2 ))
    (( jobs > $(nproc) )) && jobs=$(nproc)
    (( jobs < 1 )) && jobs=1
    echo "    ${DIM}$jobs parallel compile jobs (${mem_gb} GB RAM available); log: $log${RESET}"
    started=$SECONDS
    if ! {
        cmake -S "$SRC" -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release \
            -DGGML_NATIVE=ON -DSD_SERVER_BUILD_FRONTEND=ON &&
        cmake --build "$BUILD" --target sd-server sd-cli -j "$jobs"
    } >"$log" 2>&1; then
        tail -n 40 "$log" >&2
        die "the build failed; the full log is $log"
    fi
    echo "$COMMIT" >"$STAMP"
    ok "built in $((SECONDS - started)) s"
else
    ok "build is up to date ($COMMIT)"
fi

if [[ $BUILD_ONLY -eq 1 ]]; then
    ok "done: $SERVER_BIN"
    exit 0
fi

# ---------------------------------------------------------------- start

mkdir -p "$MODELS" "$OUTPUTS"
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
free_gb="$(df -Pk "$MODELS" | awk 'NR == 2 {printf "%d", $4 / 1048576}')"
step "Starting sd.cpp Studio"
echo "    ${DIM}models:  $MODELS${RESET}"
echo "    ${DIM}outputs: $OUTPUTS${RESET}"
echo "    ${DIM}$THREADS threads, ${ram_gb} GB RAM, ${free_gb} GB free disk${RESET}"
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

cd "$HOME_DIR"
exec "$SERVER_BIN" \
    --listen-ip "$HOST" --listen-port "$PORT" \
    --model-dir "$MODELS" --output-dir "$OUTPUTS" \
    --mmap --auto-fit off --vae-tiling -t "$THREADS"
