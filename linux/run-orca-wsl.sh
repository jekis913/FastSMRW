#!/usr/bin/env bash
# Launches the Linux app under WSLg with Orca running, for screen-reader
# testing. Forces the GTK AT-SPI bridge on (outside a full desktop session GTK
# doesn't always enable accessibility by itself) and starts Orca if it isn't
# already up, then runs the app from dist/linux.
#
# Usage:  linux/run-orca-wsl.sh
set -euo pipefail
cd "$(dirname "$0")/.."   # repo root

export GTK_MODULES=gail:atk-bridge
export NO_AT_BRIDGE=0
gsettings set org.gnome.desktop.interface toolkit-accessibility true 2>/dev/null || true

if ! pgrep -x orca >/dev/null 2>&1; then
    echo "Starting Orca..."
    setsid orca >/dev/null 2>&1 &
    sleep 3
fi

exec ./dist/linux/FastSMRW
