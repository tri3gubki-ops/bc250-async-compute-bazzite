# Changelog

## 0.3.0

Documentation only. The driver and the patches in `patches/` are unchanged, so
the 0.2.4 release binary stays correct -- no new binary is published.

- Documented how to combine this build with
  [dmorazasanchez/bc250-fsr4](https://github.com/dmorazasanchez/bc250-fsr4),
  which optimises signed packed 4x8 dot product lowering on GFX1013. Their
  patch already contains this repo's `0001`, so it replaces rather than stacks
  with it.
- That project ships no license, so its patch is referenced, not vendored, and
  the release binary does not contain it.
- One hunk of their v3 patch is already upstream in Mesa 26.2.1: GFX1013 was
  added to the `ver_minor` fix-up list in `ac_gpu_info.c`. Drop it or the patch
  will not apply.
- Measured the effect on this board with their micro-benchmark: signed dot
  665 -> 713 Gdot/s (+7.2%), unsigned and mixed unchanged, `verify: PASS` on
  both builds. Correctness was never the issue -- upstream Mesa already
  excludes `CHIP_GFX1013` from `has_accelerated_dot_product`, so the native
  `v_dot4_i32_i8` is not emitted in the first place.

## 0.2.4
- Corrected 0.2.3: `AMDGPU_GEM_OP_SET_PRIORITY` is not implemented by the
  Bazzite kernel, so `VK_EXT_pageable_device_local_memory` is advertised but
  inert — same as the stock driver. Added `test/gemop-probe.c`.

## 0.2.3
- Added `VK_EXT_pageable_device_local_memory` (patch `0003`, from OGC) for
  parity with the Mesa Bazzite ships. Device extensions: 225 → 226.

## 0.2.2
- `install.sh` now symlinks `$PREFIX/share/drirc.d` to `/usr/share/drirc.d`
  instead of installing a snapshot, so game profiles stay current.
  `$PREFIX/etc/drirc` is linked to `/etc/drirc` for the same reason.

## 0.2.1
- `status`: the "system driver" row measured the patched driver when always-on
  was enabled, because the inherited `VK_DRIVER_FILES` was not cleared.
- `status` and `bc250-game-trace`: `rpm ... | head -1` under `set -o pipefail`
  intermittently appended a bogus `unknown`.

## 0.2.0
- Patched RADV as a separate driver, helper
  (`status`/`steam`/`test`/`soak`/`trace`/`always`), `bc250-game-trace`.
  System-wide mode by default, `--per-game` keeps the old behaviour.
