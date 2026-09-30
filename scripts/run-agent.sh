#!/usr/bin/env bash
set -euo pipefail

project_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
source "$project_root/scripts/qt-env.sh"
desktop_dir="$project_root/apps/backup-desktop"
if [[ ! -x "$project_root/build/backup-agent" || ! -d "$desktop_dir/node_modules" ]]; then
    "$project_root/scripts/build.sh"
fi
npm run build --prefix "$desktop_dir"
"$project_root/scripts/install-electron.sh"
unset ELECTRON_RUN_AS_NODE
electron_lib_dir="${BACKUP_ELECTRON_LIB_DIR:-$HOME/.local/opt/backup-electron-libs/usr/lib/x86_64-linux-gnu}"
if [[ -d "$electron_lib_dir" ]]; then
    export LD_LIBRARY_PATH="$electron_lib_dir${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
fi
electron_options=()
if [[ -n "${WSL_DISTRO_NAME:-}" ]]; then
    # Use WSLg's native Wayland path for frameless windows. XWayland adds another
    # window manager and can lose input or misreport state after maximize/minimize.
    electron_platform="${BACKUP_ELECTRON_PLATFORM:-}"
    if [[ -z "$electron_platform" ]]; then
        wayland_socket="${WAYLAND_DISPLAY:-}"
        if [[ "$wayland_socket" != /* ]]; then
            wayland_socket="${XDG_RUNTIME_DIR:-}/$wayland_socket"
        fi
        if [[ -n "${WAYLAND_DISPLAY:-}" && -S "$wayland_socket" ]]; then
            electron_platform=wayland
        else
            electron_platform=x11
        fi
    fi
    case "$electron_platform" in
        wayland|x11) ;;
        *) echo 'BACKUP_ELECTRON_PLATFORM 必须是 wayland 或 x11' >&2; exit 2 ;;
    esac
    electron_options+=("--ozone-platform=$electron_platform" --disable-gpu)
fi
exec "$desktop_dir/node_modules/.bin/electron" "$desktop_dir" "${electron_options[@]}" "$@"
