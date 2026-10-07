#!/usr/bin/env bash
set -euo pipefail

project_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
source "$project_root/scripts/qt-env.sh"
data_root="${BACKUP_DATA_DIR:-$(dirname "$project_root")/BackupSystem}"
exec "$project_root/build/backup-server" --data-dir "$data_root" "$@"
