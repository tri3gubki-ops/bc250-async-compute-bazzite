# bc250-async-compute-bazzite

Async compute for the AMD BC-250 (GFX1013) on Bazzite and Fedora atomic.

Patched RADV is installed as a separate Vulkan driver in
`/usr/local/lib/bc250-radv`. The system Mesa is not replaced.

## Install

Download `bc250-async-compute-<version>.tar.zst` from
[Releases](../../releases), then:

```bash
tar --zstd -xf bc250-async-compute-*.tar.zst
cd bc250-async-compute-*/
sudo ./install.sh
```

Log out and log back in. Games need no launch options.

## Check

```bash
bc250-async-compute status
```

Should show:

```
dedicated compute (ACE) queue families:
  system driver : 0
  this package  : 1
```

## Test

```bash
bc250-async-compute test    # short
bc250-async-compute soak    # long, bit-exact
```

Record whether a game actually used the compute queues — put this in the
game's launch options:

```
bc250-game-trace %command%
```

Then:

```bash
bc250-async-compute trace
```

## Uninstall

```bash
sudo ./uninstall.sh
```

If the desktop does not come back after install, switch to a text console
with Ctrl+Alt+F3 and run:

```bash
sudo rm /etc/environment.d/95-bc250-async-compute.conf
sudo systemctl reboot
```

## Build the driver yourself

Needs `podman`. Takes 20–40 minutes.

```bash
W=$PWD/work
mkdir -p "$W"/{src,build,stage,repos}
cp /etc/yum.repos.d/*.repo "$W/repos/"

curl -fsSL -o "$W/src/mesa.tar.bz2" \
  https://gitlab.freedesktop.org/mesa/mesa/-/archive/mesa-26.2.1/mesa-mesa-26.2.1.tar.bz2
tar -xf "$W/src/mesa.tar.bz2" -C "$W/src"
mv "$W/src/mesa-mesa-26.2.1" "$W/src/mesa-26.2.1"

for p in "$PWD"/patches/*.patch; do
  ( cd "$W/src/mesa-26.2.1" && git apply -p1 "$p" ) || exit 1
done

podman run --rm \
  -v "$W/repos:/repos:ro,Z" -v "$W/src:/src:Z" \
  -v "$W/build:/build:Z" -v "$W/stage:/stage:Z" \
  -v "$PWD/build/build-radv.sh:/build-radv.sh:ro,Z" \
  registry.fedoraproject.org/fedora:44 bash /build-radv.sh

mkdir -p payload
cp -r "$W/stage/usr/local/lib/bc250-radv/"* payload/
sudo ./install.sh
```

`--prefix` must stay `/usr/local/lib/bc250-radv`: Mesa bakes that path into
the ICD manifest and the drirc directory at configure time.

### Optional: faster signed integer dot products

[dmorazasanchez/bc250-fsr4](https://github.com/dmorazasanchez/bc250-fsr4)
optimises how RADV lowers signed packed 4x8 dot products on GFX1013. Upstream
Mesa already refuses the native `v_dot4_i32_i8` on this chip
(`has_accelerated_dot_product` excludes `CHIP_GFX1013`), so results are correct
either way -- their patch makes the software path cheaper.

Measured here with their own micro-benchmark, Mesa 26.2.1, this board:

| Operation | Without | With | |
|---|---|---|---|
| signed | 665 Gdot/s | 713 Gdot/s | +7.2% |
| unsigned | 666.54 | 666.57 | unchanged |
| mixed | 665.22 | 664.90 | unchanged |

`verify: PASS` on both.

That project ships **no license**, so its patch is not vendored here. Fetch it
yourself, before this repo's patches:

```bash
git clone --depth 1 -b v3 https://github.com/dmorazasanchez/bc250-fsr4 /tmp/fsr4
# drop the one hunk already upstream in 26.2.1 (GFX1013 in the ver_minor list)
grep -v -A9 '^@@ -504,7 +502,8 @@' /tmp/fsr4/bc250-fsr4-v3.patch > /tmp/fsr4-26.2.1.patch
( cd "$W/src/mesa-26.2.1" && git apply /tmp/fsr4-26.2.1.patch )
```

Their patch already contains this repo's `0001`, so apply only `0003` after it.

## Requirements

- AMD BC-250
- Bazzite 44 or Fedora 44 atomic
- kernel 7.2.0-ogc4.1 or newer (ships with Bazzite)

## What is patched

Three patches against Mesa 26.2.1, all in `patches/`:

| Patch | Effect |
|---|---|
| `0001` | exposes the dedicated compute (ACE) queue on GFX1013 and routes the chip through the existing Iceland/Tonga threadgroup workaround |
| `0002` | optional queue-count debug output, `BC250_IP_DEBUG=1` |
| `0003` | `VK_EXT_pageable_device_local_memory`, for parity with the Mesa that Bazzite ships |

Optional and not vendored: the signed-dot lowering work above.

## Known limits

- No kernel patches are applied. This was tested only on the OGC kernel that
  Bazzite ships (`7.2.0-ogc4.1`, `7.2.0-ogc6.1`). On other kernels follow the
  upstream project, which requires its kernel half.
- `VK_EXT_pageable_device_local_memory` needs `AMDGPU_GEM_OP_SET_PRIORITY` in
  the kernel. That op is absent from the Bazzite kernel, so the extension is
  advertised but does nothing — same as the stock driver. `test/gemop-probe.c`
  checks this.
- `dEQP-VK.synchronization2` has not been run.

## Credits

| Work | Source |
|---|---|
| Opening the ACE queue on GFX1013, threadgroup workaround | [DryhoppedIPA/bc250-gfx1013-fix](https://github.com/DryhoppedIPA/bc250-gfx1013-fix) |
| `VK_EXT_pageable_device_local_memory` (`0003`) | Natalie Vock, via [OpenGamingCollective/mesa](https://github.com/OpenGamingCollective/mesa) |
| RADV | [Mesa](https://gitlab.freedesktop.org/mesa/mesa), MIT |
| Signed packed-dot lowering (optional, not vendored) | [dmorazasanchez/bc250-fsr4](https://github.com/dmorazasanchez/bc250-fsr4) |
| INT8 dot micro-benchmark used for the numbers above | same project, originally PR #1 by `higorprado` |

Not affiliated with AMD, Valve, Fyra Labs, the Open Gaming Collective or the
Mesa project.

## License

MIT, see [LICENSE](LICENSE). The patches are derived from Mesa and carry Mesa's
MIT terms.
