# Installer (Setup PKG, `ORPC00001`)

## What it does

One linear flow, forward-only (every No skips ahead, nothing loops back):
confirm → copy `orbisrpc.bin` to `/data/GoldHEN/bin/elf/` (mkdir -p +
byte-count + hash read-back, one OK/denied line) → token prompt (skipped
when valid) → done. Declining install offers a read-only status screen
instead.

There is no WiFi check. The installer runs sandboxed, so its socket probe
measures the app's network stack, not the payload's — and a FAILED line
reads like a verdict on Discord itself. The daemon reports real reachability
in its own log once started.

The installer never boots anything. Starting the daemon is GoldHEN's
payload menu or AutoRun — see below. No loopback ports, no eviction, no
boot proof to go wrong.

## Navigation law

No branch ever returns to start. Cancel/skip always moves forward. The only
exits are Done, explicit close, or a payload write failure.

## Console facts encoded

- Splash must hide before dialogs (`sceSystemServiceHideSplashScreen`) +
  3 s foreground settle, or the first dialog flash-dismisses.
- Dialog/IME/IME-backend sysmodules + internal SYSTEM/USER/COMMON modules
  load before use (unloaded calls panic).
- `stat()`/`fstat()` lie about sizes inside the sandbox (observed 4096/4160
  for a 2071552-byte file), so install proof is byte count + FNV hash on
  read-back, and status uses open-existence, never stat.
- Config writes are atomic (tmp+fsync+rename) with read-back proof.
- IME wait is bounded; asset key charset validated (bad keys blank the
  activity).

## Auto-start

GoldHEN PayLoader AutoRun (2.4b18.10+): enable it for `orbisrpc` once and
the daemon starts on every jailbreak. The queue is menu-managed; the
installer shows where, it can't write the queue itself.

## Building

`make -f installer/Makefile` (`OO_PS4_TOOLCHAIN`, llvmshim). Staged assets:
daemon + evict ELFs — the installer only reads the daemon, evict rides along
for manual injection. Output: `IV0000-ORPC00001_00-ORBISRPCSETUP000.pkg`.
