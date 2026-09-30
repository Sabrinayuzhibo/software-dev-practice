#!/usr/bin/env bash
set -euo pipefail

project_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
electron_dir="$project_root/apps/backup-desktop/node_modules/electron"
if [[ -x "$electron_dir/dist/electron" && -f "$electron_dir/path.txt" ]]; then
    exit 0
fi
if [[ ! -d "$electron_dir" ]]; then
    echo "请先运行 ./scripts/build.sh 安装前端依赖" >&2
    exit 1
fi

version="$(node -e "process.stdout.write(require(process.argv[1]).version)" "$electron_dir/package.json")"
archive="electron-v${version}-linux-x64.zip"
expected="$(node -e "process.stdout.write(require(process.argv[1])[process.argv[2]] || '')" "$electron_dir/checksums.json" "$archive")"
if [[ -z "$expected" || "$expected" == "undefined" ]]; then
    echo "无法取得 Electron 下载校验值" >&2
    exit 1
fi

temporary="$(mktemp -d)"
trap 'rm -rf "$temporary"' EXIT
echo "下载 Electron ${version} 运行时…" >&2
curl -fL --retry 2 --connect-timeout 15 \
    "https://github.com/electron/electron/releases/download/v${version}/${archive}" \
    -o "$temporary/$archive"
printf '%s  %s\n' "$expected" "$temporary/$archive" | sha256sum --check --status
mkdir -p "$electron_dir/dist"
unzip -q "$temporary/$archive" -d "$electron_dir/dist"
printf electron > "$electron_dir/path.txt"
