#!/usr/bin/env bash
# Use an optional user-local Qt installation when system packages are unavailable.

qt_prefix="${BACKUP_QT_PREFIX:-$HOME/.local/opt/backup-qt6}"
qt_usr="$qt_prefix/usr"
qt_lib="$qt_usr/lib/x86_64-linux-gnu"
if [[ -f "$qt_lib/pkgconfig/Qt6Core.pc" ]]; then
    export PATH="$qt_usr/bin:$qt_usr/lib/qt6/bin:$PATH"
    export PKG_CONFIG_PATH="$qt_lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
    export PKG_CONFIG_SYSROOT_DIR="$qt_prefix"
    export LD_LIBRARY_PATH="$qt_lib:$qt_lib/libproxy${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
    export QT_QPA_PLATFORM_PLUGIN_PATH="$qt_lib/qt6/plugins/platforms"
fi

# WSLg exposes X11 alongside Wayland; use XCB even if the shell selected Wayland.
if [[ -n "${WSL_DISTRO_NAME:-}" && -n "${DISPLAY:-}" ]]; then
    export QT_QPA_PLATFORM="${BACKUP_QT_PLATFORM:-xcb}"
fi
