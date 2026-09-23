# TLS + IME console findings

## TLS: mbedTLS 3.x PSA failure (FIXED, verified on hardware)

Symptom: every handshake died instantly with return `-1` (not a real
mbedTLS code), after DNS + TCP succeeded.

Root cause (from mbedTLS debug trace on-console):

```text
psa_crypto_init() returned -148 (-0x0094, INSUFFICIENT_ENTROPY)
```

mbedTLS 3.x routes TLS 1.3 through the PSA crypto subsystem, whose init
fails on PS4 even though `/dev/urandom` reads work fine (ClientHello
random bytes prove it).

Fix (in tree, verified: full handshake, `TLS-ECDHE-ECDSA-WITH-CHACHA20-
POLY1305-SHA256`, cert chain verifies, connection test PASSES):

```c
mbedtls_ssl_conf_max_tls_version(&t->conf, MBEDTLS_SSL_VERSION_TLS1_2);
```

Lesson: wire mbedTLS's debug layer (`mbedtls_debug_set_threshold` +
`mbedtls_ssl_conf_dbg`) into the log before guessing at handshake bugs.

## IME keyboard: init refuses (OPEN)

`sceImeDialogInit` fails on-console; codes seen:

- `-2135162872` (`0x80BC0008`) with `userId=0`
- `-2135162864` (`0x80BC0010`) with real foreground user ID

Tried, none fixed it: real user ID via UserService, centered position
(960,540), default type/label, per-dialog init/terminate lifecycle,
stale-session teardown. Reference: Apollo PS4 dialog.c pattern mirrored.
Workaround in place: token pre-seeded via config; IME still unresolved.

## App lifecycle (fixed, verified)

- Missing `sceMsgDialogInitialize` → every open failed into a black loop.
  Fixed per Apollo pattern (init + terminate per dialog).
- 30s dialog watchdog blanked idle screens — removed, blocking waits now.
- Exit path terminates dialogs, unloads modules, `exit(0)`.
