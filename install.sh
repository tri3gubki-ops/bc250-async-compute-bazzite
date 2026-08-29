#!/usr/bin/env bash
# Install the patched RADV payload for BC-250 async compute.
#
# Nothing about the system Mesa is touched. Everything lands under /usr/local,
# which on atomic systems (Bazzite, Silverblue, Kinoite) is a symlink to
# /var/usrlocal: writable, persistent across image updates, and it needs no
# rpm-ostree layering and no new deployment.
set -euo pipefail

PREFIX=/usr/local/lib/bc250-radv
BINDIR=/usr/local/bin
SHARE=/usr/local/share/bc250-async-compute
BC250_PCI="1002:13fe"
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)

if [ -t 1 ]; then B=$'\033[1m'; R=$'\033[0m'; G=$'\033[32m'; Y=$'\033[33m'; E=$'\033[31m'
else B=""; R=""; G=""; Y=""; E=""; fi
ok()   { printf '%s[ OK ]%s %s\n' "$G" "$R" "$*"; }
warn() { printf '%s[WARN]%s %s\n' "$Y" "$R" "$*"; }
die()  { printf '%s[FAIL]%s %s\n' "$E" "$R" "$*"; exit 1; }

FORCE=0
YES=0
# Default: make the patched driver the system Vulkan driver, so games need no
# launch options. --per-game leaves it inert until VK_DRIVER_FILES is set.
ALWAYS=1
for a in "$@"; do
	case "$a" in
	--force) FORCE=1 ;;
	-y|--yes) YES=1 ;;
	--per-game) ALWAYS=0 ;;
	-h|--help) sed -n '2,12p' "$0"; exit 0 ;;
	*) die "unknown option $a" ;;
	esac
done

[ "$(id -u)" = 0 ] || die "run as root: sudo ./install.sh"

# --- guards -----------------------------------------------------------------

# Capture first, then match: with `set -o pipefail`, `cmd | grep -q` reports
# failure whenever grep exits on its first match and the producer gets SIGPIPE.
pci_list=$(lspci -nn 2>/dev/null || true)
if ! printf '%s' "$pci_list" | grep -qi "\[$BC250_PCI\]"; then
	[ "$FORCE" = 1 ] || die "AMD BC-250 ($BC250_PCI) not found. This payload is GFX1013-only. Use --force to override."
	warn "BC-250 not detected, continuing because --force was given"
else
	ok "board: AMD BC-250 detected"
fi

[ -d /sys/module/amdgpu ] || warn "amdgpu is not loaded; the driver will be useless until it is"

[ -f "$HERE/payload/lib64/libvulkan_radeon.so" ] || die "payload missing from this package directory"

# The payload links against system libdrm/wayland/xcb. Refuse to install a
# driver that cannot resolve its libraries rather than leaving a broken ICD.
missing=$(ldd "$HERE/payload/lib64/libvulkan_radeon.so" 2>/dev/null | awk '/not found/{print $1}' || true)
if [ -n "$missing" ]; then
	die "payload cannot resolve: $(echo "$missing" | tr '\n' ' ')
Build it for this distribution instead: see build/README.md"
fi
ok "all shared library dependencies resolve on this system"

if [ "$YES" != 1 ]; then
	cat <<PLAN

${B}About to install:${R}
  $PREFIX/lib64/libvulkan_radeon.so      patched RADV (private, not a system driver)
  $PREFIX/share/vulkan/icd.d/            its ICD manifest, outside every loader search path
  $PREFIX/share/drirc.d/                 symlink to /usr/share/drirc.d (game profiles)
  $PREFIX/etc/drirc                      symlink to /etc/drirc (your own overrides)
  $BINDIR/bc250-async-compute            helper: status / steam / test / soak / trace
  $BINDIR/bc250-game-trace               launch-options wrapper that records a run
  $BINDIR/bc250-trace-report             turns a recorded run into a verdict
  $BINDIR/bc250-trace-sampler            1 Hz engine-time and sensor sampler
  $SHARE/                                patches, tests, docs, provenance

The system Mesa package is not modified or removed. By default the patched
driver is then selected for every session through /etc/environment.d, so games
need no launch options -- including the desktop compositor. Pass --per-game to
skip that and keep it inert until VK_DRIVER_FILES points at it.

PLAN
	printf 'Continue? [y/N] '
	read -r reply
	case "$reply" in y|Y|yes|YES) ;; *) die "cancelled" ;; esac
fi

# --- install ----------------------------------------------------------------

install -d -m 755 "$PREFIX/lib64" "$PREFIX/share/vulkan/icd.d" "$SHARE"
install -m 644 "$HERE/payload/lib64/libvulkan_radeon.so" "$PREFIX/lib64/"
install -m 644 "$HERE/payload/share/vulkan/icd.d/"*.json "$PREFIX/share/vulkan/icd.d/"

# Mesa bakes its drirc paths in at configure time, so this driver reads game
# profiles from $PREFIX/share/drirc.d. Link that to the system directory,
# otherwise the profiles freeze at build time.
rm -rf "$PREFIX/share/drirc.d"
if [ -d /usr/share/drirc.d ]; then
	ln -sfn /usr/share/drirc.d "$PREFIX/share/drirc.d"
	echo "  drirc.d -> /usr/share/drirc.d (game profiles stay current)"
else
	install -d -m 755 "$PREFIX/share/drirc.d"
	install -m 644 "$HERE/payload/share/drirc.d/"*.conf "$PREFIX/share/drirc.d/"
	echo "  drirc.d: no system directory, using the bundled snapshot"
fi

# Same for /etc/drirc. A dangling link is harmless.
install -d -m 755 "$PREFIX/etc"
ln -sfn /etc/drirc "$PREFIX/etc/drirc"
install -m 755 "$HERE/bc250-async-compute" "$BINDIR/bc250-async-compute"
install -m 755 "$HERE/bc250-game-trace"    "$BINDIR/bc250-game-trace"
install -m 755 "$HERE/bc250-trace-report"  "$BINDIR/bc250-trace-report"
install -m 755 "$HERE/bc250-trace-sampler" "$BINDIR/bc250-trace-sampler"

cp -a "$HERE/patches" "$HERE/test" "$SHARE/"
install -m 644 "$HERE/README.md" "$SHARE/"
[ -f "$HERE/README.ru.md" ] && install -m 644 "$HERE/README.ru.md" "$SHARE/"
[ -f "$HERE/VERSION" ] && install -m 644 "$HERE/VERSION" "$SHARE/"
[ -d "$HERE/build" ] && cp -a "$HERE/build" "$SHARE/"

# SELinux: /usr/local is var_usrlocal_t underneath; relabel so the loader and
# the helper are readable/executable under the default policy.
if command -v restorecon >/dev/null 2>&1 && [ "$(getenforce 2>/dev/null || echo Disabled)" != Disabled ]; then
	restorecon -RF "$PREFIX" "$BINDIR/bc250-async-compute" "$BINDIR/bc250-game-trace" \
		"$BINDIR/bc250-trace-report" "$BINDIR/bc250-trace-sampler" "$SHARE" 2>/dev/null || true
	ok "SELinux labels restored"
fi

ok "installed"

if [ "$ALWAYS" = 1 ]; then
	"$BINDIR/bc250-async-compute" always enable
else
	warn "--per-game: async compute stays inert until VK_DRIVER_FILES is set"
	printf '       turn it on later: sudo bc250-async-compute always enable\n'
fi

printf '\n%sNext:%s\n  bc250-async-compute status\n\n' "$B" "$R"

