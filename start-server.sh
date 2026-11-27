#!/bin/sh
set -eu

SD_SERVER_PATH="${SD_SERVER_PATH:-}"
MODEL_DIR="./models"
COMMANDLINE_ARGS=""

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)

# Opened by a double-click in a file manager: there is no terminal, so the server would run
# invisibly and errors would be lost. Reopen this script in a terminal window instead, from
# its own folder so ./models resolves, and keep the window open after the server stops.
if [ ! -t 0 ] && [ ! -t 1 ] && [ -z "${SD_START_SERVER_IN_TERMINAL:-}" ] &&
    [ -n "${DISPLAY:-}${WAYLAND_DISPLAY:-}" ]; then
    export SD_START_SERVER_IN_TERMINAL=1
    hold='"$0" "$@"; status=$?; printf "\nsd-server stopped (exit code %s). Press Enter to close this window.\n" "$status"; read -r _'
    self="$script_dir/$(basename -- "$0")"
    cd "$script_dir"
    for terminal in gnome-terminal kgx konsole xfce4-terminal mate-terminal tilix x-terminal-emulator alacritty kitty xterm; do
        command -v "$terminal" >/dev/null 2>&1 || continue
        case "$terminal" in
            gnome-terminal | kgx | tilix | mate-terminal) exec "$terminal" -- sh -c "$hold" "$self" "$@" ;;
            xfce4-terminal) exec "$terminal" -x sh -c "$hold" "$self" "$@" ;;
            *) exec "$terminal" -e sh -c "$hold" "$self" "$@" ;;
        esac
    done
fi

if [ -f "$script_dir/start-server.local.sh" ]; then
    . "$script_dir/start-server.local.sh"
fi

if [ "${1:-}" != "--version" ]; then
    # Split configured arguments on whitespace without expanding wildcards.
    set -f
    set -- --model-dir "$MODEL_DIR" $COMMANDLINE_ARGS "$@"
fi

run_server() {
    printf 'Running:'
    for arg do
        printf " '%s'" "$(printf '%s' "$arg" | sed "s/'/'\\\\''/g")"
    done
    printf '\n'
    exec "$@"
}

if [ -n "$SD_SERVER_PATH" ]; then
    case "$SD_SERVER_PATH" in
        */*) ;;
        *) SD_SERVER_PATH="./$SD_SERVER_PATH" ;;
    esac
    if [ ! -f "$SD_SERVER_PATH" ] || [ ! -x "$SD_SERVER_PATH" ]; then
        printf 'SD_SERVER_PATH is not an executable file: %s\n' "$SD_SERVER_PATH" >&2
        exit 1
    fi
    run_server "$SD_SERVER_PATH" "$@"
fi

for server in \
    "$script_dir/sd-server" \
    "$script_dir/bin/Release/sd-server" \
    "$script_dir/bin/sd-server" \
    "$script_dir/build/bin/Release/sd-server" \
    "$script_dir/build/bin/sd-server"
do
    if [ -f "$server" ] && [ -x "$server" ]; then
        run_server "$server" "$@"
    fi
done

printf '%s\n' 'sd-server was not found. Build it first or set SD_SERVER_PATH to its executable path.' >&2
exit 1
