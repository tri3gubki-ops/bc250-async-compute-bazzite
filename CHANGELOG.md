# Changelog

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
