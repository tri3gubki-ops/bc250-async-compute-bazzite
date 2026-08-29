#!/usr/bin/env bash
# Rebuild the two test binaries. Needs a container runtime (podman) or, if the
# host already has gcc + vulkan headers + glslc, run with BC250_NATIVE=1.
set -euo pipefail
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)

build() {
   cd "$HERE"
   glslc -O --target-env=vulkan1.1 -fshader-stage=compute ace.comp -o ace.spv
   python3 - <<'PY'
data = open('ace.spv','rb').read()
assert len(data) % 4 == 0
words = ', '.join('0x%08x' % int.from_bytes(data[i:i+4],'little') for i in range(0,len(data),4))
open('ace_comp_spv.h','w').write(
  '/* generated from ace.comp by glslc -- do not edit */\n'
  '#include <stdint.h>\n'
  'static const uint32_t ace_comp_spv[] = {%s};\n' % words)
PY
   gcc -O2 -Wall -o bc250-ace-test bc250-ace-test.c -lvulkan
   gcc -O2 -Wall -o bc250-ace-soak bc250-ace-soak.c -lvulkan
   echo "built: bc250-ace-test bc250-ace-soak"
}

if [ "${BC250_NATIVE:-0}" = 1 ]; then
   build
   exit 0
fi

command -v podman >/dev/null || { echo "podman not found; install gcc/glslc/vulkan-headers and re-run with BC250_NATIVE=1"; exit 1; }
podman run --rm -v "$HERE:/test:Z" registry.fedoraproject.org/fedora:44 bash -c '
  set -e
  dnf -y -q install gcc glslc vulkan-headers vulkan-loader-devel python3 >/dev/null 2>&1
  cd /test && BC250_NATIVE=1 ./build.sh'
