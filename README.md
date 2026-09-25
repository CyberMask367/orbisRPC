# orbisRPC — Discord Rich Presence for the PS4

A background daemon that runs **entirely on your jailbroken PS4** and posts
what you're playing to your Discord profile — game name, cover art, elapsed
timer. No PC, no phone bridge at runtime. v1.0.0.

## Install (5 minutes, one try)

1. Install `OrbisRPC-Setup-1.0.0.pkg` (Releases page) via Package Installer.
2. Open **orbisRPC Setup** on the home screen.
3. Say Yes: it copies the daemon everywhere loaders look, checks WiFi,
   asks for your Discord token, saves it, and starts the daemon.
4. Launch a game. Watch Discord.

After a reboot, re-jailbreak, then enable AutoRun for `orbisrpc` in
GoldHEN's payload menu — it starts itself on every jailbreak from then on.

## How it works

```
PS4 (GoldHEN)                              Discord
+----------------------------------+      +------------------+
| orbisRPC daemon (payload)        |      | your profile     |
|  sandbox scan -> CUSA id         |  TLS | Playing Spider-  |
|  app.db -> display name          | <--> | Man — 1h 23m     |
|  art pack/CDN -> mp: cover       |      | [cover] [timer]  |
+----------------------------------+      +------------------+
```

- **Detection:** the running game's `/mnt/sandbox` mount + eboot fast-switch
  + 2-poll debounce. No tables, no per-game setup — any title works.
- **Names:** system app.db (SQLite, read-only) → local files → Sony TMDB
  live → raw ID fallback. First authoritative hit self-learns into config.
- **Home:** PlayStation logo tile with "On PS4" when idle.
- Full design: `docs/DAEMON.md`. Installer: `docs/INSTALLER.md`.
  Symptoms table: `docs/TROUBLESHOOTING.md`.

## Config (`/data/orbisRPC/config.json`)

Only `token` is required (your Discord user session token). Everything else
has working defaults: `presence_state`, `home_art`, `poll_interval_s`,
`application_id`, `debug`, plus the self-learned `titles` map (hands off —
the daemon maintains it).

## Building

Daemon + tools: `./scripts/build_sdk.sh` (needs `ps4-payload-sdk`).
Installer PKG: `make -f installer/Makefile`
(`OO_PS4_TOOLCHAIN`, llvmshim — no brew). Host tests:
`make -C tests test && make -C tests asan`, contracts:
`python3 tests/e2e_consumer.py`.

## Safety

A user session token grants full account access: never share the config,
never commit a real token. Token use for presence is against Discord's ToS
(Standard practice for headless presence tools; risk is yours — see docs).

## License

Project license to be finalized (MIT vs GPL). No GPL source is copied into
this tree; the OpenOrbis toolchain it builds against is GPL-3.0.
