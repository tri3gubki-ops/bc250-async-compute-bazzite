#!/bin/bash
# Build the patched RADV payload. PREFIX must equal the final install path:
# Mesa bakes DATADIR (drirc.d) and the ICD library_path in at configure time.
set -e
PREFIX=${PREFIX:-/usr/local/lib/bc250-radv}
cp /repos/*.repo /etc/yum.repos.d/ 2>/dev/null || true
dnf -y -q install meson ninja-build gcc gcc-c++ python3-mako python3-packaging \
    libdrm-devel wayland-devel wayland-protocols-devel libX11-devel \
    libxcb-devel xcb-util-keysyms-devel libXrandr-devel libxshmfence-devel \
    zlib-devel expat-devel bison flex glslang spirv-tools cbindgen \
    rust-packaging clang-devel llvm-devel python3-pyyaml git >/dev/null 2>&1
cd /src/mesa-26.2.1
rm -rf /build/b && meson setup /build/b --prefix="$PREFIX" --buildtype=release \
  -Dvulkan-drivers=amd -Dgallium-drivers= -Dplatforms=wayland,x11 \
  -Dglx=disabled -Degl=disabled -Dgbm=disabled -Dopengl=false \
  -Dgles1=disabled -Dgles2=disabled -Dllvm=disabled \
  -Dvideo-codecs= -Dvulkan-layers= -Dtools= -Dshared-glapi=disabled \
  > /build/setup.log 2>&1 || { tail -40 /build/setup.log; exit 1; }
ninja -C /build/b > /build/ninja.log 2>&1 || { tail -50 /build/ninja.log; exit 1; }
DESTDIR=/stage ninja -C /build/b install > /build/install.log 2>&1 || { tail -20 /build/install.log; exit 1; }
echo "BUILD OK prefix=$PREFIX"
find /stage -type f | sed 's|^/stage||'
