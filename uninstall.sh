#!/usr/bin/env bash
# Remove everything install.sh added. The system Mesa was never touched, so
# uninstalling is enough to return to stock behaviour.
set -euo pipefail

PREFIX=/usr/local/lib/bc250-radv
BINDIR=/usr/local/bin
SHARE=/usr/local/share/bc250-async-compute

[ "$(id -u)" = 0 ] || { echo "run as root: sudo ./uninstall.sh"; exit 1; }

# Deactivate FIRST, remove the driver second. The other order leaves a window
# where every session points VK_DRIVER_FILES at a driver that is already gone,
# and an interruption there would break Vulkan system-wide.
for f in /etc/environment.d/95-bc250-async-compute.conf \
         /etc/profile.d/95-bc250-async-compute.sh; do
	if [ -f "$f" ]; then rm -f "$f"; echo "removed $f"; fi
done

# 0.1.0 put this in each user's home.
for home in /home/* /var/home/*; do
	conf="$home/.config/environment.d/95-bc250-async-compute.conf"
	[ -f "$conf" ] && rm -f "$conf" && echo "removed $conf"
done

for d in "$PREFIX" "$SHARE"; do
	if [ -d "$d" ]; then rm -rf "$d"; echo "removed $d"; fi
done
for b in bc250-async-compute bc250-game-trace bc250-trace-report bc250-trace-sampler; do
	if [ -f "$BINDIR/$b" ]; then rm -f "$BINDIR/$b"; echo "removed $BINDIR/$b"; fi
done

echo
echo "done -- log out and back in so the stock driver takes over"
echo "also clear VK_DRIVER_FILES from any Steam launch options you set by hand"
