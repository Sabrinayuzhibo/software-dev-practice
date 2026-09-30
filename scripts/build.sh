#!/usr/bin/env bash
set -euo pipefail

project_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
source "$project_root/scripts/qt-env.sh"
build_dir="$project_root/build"
if [[ ! -f "$build_dir/build.ninja" ]]; then
    meson setup "$build_dir" "$project_root" --backend=ninja
fi
ninja -C "$build_dir"
meson test -C "$build_dir" --print-errorlogs

desktop_dir="$project_root/apps/backup-desktop"
if [[ ! -d "$desktop_dir/node_modules" ]]; then
    npm ci --prefix "$desktop_dir"
fi
npm run build --prefix "$desktop_dir"
