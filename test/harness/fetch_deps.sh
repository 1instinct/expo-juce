#!/usr/bin/env bash
# Fetches the dev headers JUCE's gui modules need on Linux into a local sysroot,
# WITHOUT root: `apt-get download` + `dpkg -x`. Runtime .so files (libX11, libfreetype)
# are already present on stock Ubuntu 24.04; JUCE dlopens X11 and we never open a window.
set -euo pipefail
dest="$1"
[ -f "$dest/.done" ] && exit 0
mkdir -p "$dest/debs"
cd "$dest/debs"
apt-get download libfreetype-dev libx11-dev x11proto-dev libxext-dev libxrandr-dev \
    libxinerama-dev libxcursor-dev libxrender-dev libxau-dev libxcb1-dev libxfixes-dev >/dev/null
for d in *.deb; do dpkg -x "$d" "$dest"; done
touch "$dest/.done"
