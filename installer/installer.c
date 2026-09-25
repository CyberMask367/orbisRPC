/* installer.c - orbisRPC Setup, stripped to the bone:
 * install files -> wifi check -> token -> done.
 * One confirm up front; everything after flows forward.
 *
 * Payload placement is the whole job: one copy of orbisrpc.bin into
 * /data/GoldHEN/bin/elf. Booting it is GoldHEN's payload menu (or
 * AutoRun), never this app - an installer that also injects fails in
 * ways the user cannot fix from the wizard. */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/stat.h>
#include <errno.h>
#include <time.h>
#include <orbis/libkernel.h>
#include <orbis/SystemService.h>
#include "ui.h"
#include "icfg.h"
#include "nettest.h"

#ifndef SETUP_VERSION
#define SETUP_VERSION "1.0.0"
#endif
#define DAEMON_ELF "/app0/assets/daemon.elf"
#define INST_DIR "/data/orbisRPC"
/* The one place the payload lives: GoldHEN's bin/elf loader directory. */
#define PAYLOAD_BIN "/data/GoldHEN/bin/elf/orbisrpc.bin"

/* Preflight: the payload must exist inside this package. A PKG built
 * or installed without staged assets fails here with a precise message
 * instead of a mysterious write error three steps later. */
static int step_assets(void){
    FILE *a = fopen(DAEMON_ELF, "rb");
    if(a) fclose(a);
    if(!a){
        ui_ok("This install is missing its payload.\n\nReinstall the Setup PKG (do not just relaunch the old bubble), then open it again.");
        return -1;
    }
    return 0;
}

static int copy_file(const char *src, const char *dst){    FILE *in, *out;
    static unsigned char buf[65536];
    size_t n;
    long expect = -1, got;
    struct stat st;
    in = fopen(src, "rb");
    if(!in) return -1;
    if(fstat(fileno(in), &st) == 0) expect = (long)st.st_size;
    out = fopen(dst, "wb");
    if(!out){ fclose(in); return -1; }
    while((n = fread(buf, 1, sizeof buf, in)) > 0){
        if(fwrite(buf, 1, n, out) != n){ fclose(in); fclose(out); return -1; }
    }
    fclose(in);
    if(fclose(out) != 0) return -1;
    /* Read-back proof: a short write must never pass as installed. */
    if(expect > 0 && stat(dst, &st) == 0) got = (long)st.st_size;
    else got = -1;
    return (expect > 0 && got == expect) ? 0 : -1;
}

/* mkdir -p (parents as needed). 0 ok or already there. */
static int mkdirs(const char *path){
    char tmp[256];
    size_t i, n;
    if(!path || !path[0]) return -1;
    n = strlen(path);
    if(n >= sizeof tmp) return -1;
    memcpy(tmp, path, n + 1);
    for(i = 1; i < n; i++){
        if(tmp[i] == '/'){
            tmp[i] = 0;
            mkdir(tmp, 0777);
            tmp[i] = '/';
        }
    }
    if(mkdir(path, 0777) != 0 && errno != EEXIST) return -1;
    return 0;
}

static int step_files(void){
    char report[512];
    int ok = -1;
    mkdir(INST_DIR, 0777);
    if(mkdirs("/data/GoldHEN/bin/elf") == 0)
        ok = copy_file(DAEMON_ELF, PAYLOAD_BIN);
    snprintf(report, sizeof report,
             "Daemon payload:\n%s : %s\n\n"
             "Start it from GoldHEN's payload menu (bin/elf), or enable\n"
             "AutoRun for orbisrpc once and it boots with every jailbreak.",
             PAYLOAD_BIN, ok == 0 ? "OK" : "denied");
    ui_ok(report);
    if(ok != 0){
        ui_ok("Install failed: could not write the payload.\n\nStopping here.");
        return -1;
    }
    /* Config template when missing; never clobbers learned titles. */
    {
        char tok[160];
        tok[0] = 0;
        icfg_token_load(ICFG_PATH, tok, sizeof tok);
        if(!token_valid(tok)){
            FILE *f = fopen(ICFG_PATH, "rb");
            if(!f){
                f = fopen(ICFG_PATH, "wb");
                if(f){
                    fputs("{\"schema_version\":1,\"token\":\"SET_ME\",\"presence_state\":\"On PS4\"}", f);
                    fclose(f);
                }
            } else {
                fclose(f);
            }
        }
    }
    return 0;
}

/* 2: wifi. Always continues forward; result is information. */
static void step_wifi(void){
    int ok;
    if(ui_progress_open("Checking connection...") != 0) return;
    ok = net_probe("gateway.discord.gg", 443, 6);
    ui_progress_close();
    ui_ok(ok ? "WiFi check: OK.\n\nDiscord is reachable from this console."
             : "WiFi check: FAILED.\n\nDiscord is unreachable. Presence will not work until the network does.\nContinuing anyway.");
}

/* 3: token. Skips silently when one already validates. */
static void step_token(void){
    char tok[160];
    int tries, r;
    tok[0] = 0;
    icfg_token_load(ICFG_PATH, tok, sizeof tok);
    if(token_valid(tok)){
        ui_ok("A valid token is already saved.\n\nSkipping.");
        return;
    }
    for(tries = 0; tries < 3; tries++){
        r = ui_input("Discord token", "paste token, Done to save", tok, sizeof tok);
        if(r < 0){ ui_ok("Text input failed. Skipping."); return; }
        if(r == 0) return;
        if(token_valid(tok)){
            r = icfg_token_save(ICFG_PATH, tok);
            ui_ok(r == 0 ? "Token saved." : "Could not write config.");
            return;
        }
        ui_ok("That doesn't look like a token.\nCheck it and try again, or cancel to skip.");
    }
}

/* Status: read-only health snapshot for debugging. Never mutates. */
static void step_status(void){
    char out[640];
    size_t used = 0;
    char tok[160];
    struct stat st;
    tok[0] = 0;
    icfg_token_load(ICFG_PATH, tok, sizeof tok);
    used = (size_t)snprintf(out, sizeof out, "orbisRPC status:\n");
    used += (size_t)snprintf(out + used, sizeof out - used,
        "\ndaemon files: %s", stat(PAYLOAD_BIN, &st) == 0 ? "installed" : "missing");
    used += (size_t)snprintf(out + used, sizeof out - used,
        "\nlock: %s", stat("/data/orbisRPC/daemon.lock", &st) == 0 ? "held (may be running)" : "free");
    if(stat("/data/orbisRPC/log.txt", &st) == 0){
        long age = (long)time(NULL) - (long)st.st_mtime;
        used += (size_t)snprintf(out + used, sizeof out - used,
            "\nlog: present, last write %lds ago", age < 0 ? 0 : age);
    } else {
        used += (size_t)snprintf(out + used, sizeof out - used, "\nlog: none yet");
    }
    used += (size_t)snprintf(out + used, sizeof out - used,
        "\ntoken: %s", token_valid(tok) ? "saved" : "missing/invalid");
    used += (size_t)snprintf(out + used, sizeof out - used,
        "\nlearned titles: %d", icfg_titles_count(ICFG_PATH));
    if(used >= sizeof out - 64) out[sizeof out - 64] = 0;
    ui_ok(out);
}

int main(void){
    int q;
    char welcome[256];
    if(ui_init() != 0) return 1;
    /* The system holds a splash screen over fresh apps: dialogs opened
     * under it get auto-dismissed (the half-second flash) or never
     * surface. Hide it once the UI layer is ready, then let the
     * foreground transition settle before the first dialog. */
    sceSystemServiceHideSplashScreen();
    sceKernelSleep(3);
    snprintf(welcome, sizeof welcome,
             "orbisRPC Setup %s\n\nInstalls the daemon payload, checks WiFi, and saves your token.",
             SETUP_VERSION);
    q = ui_confirm(welcome);
    if(q != 1){
        /* Declined install: offer status, then exit. Forward only. */
        if(ui_confirm("Show daemon status instead?") == 1)
            step_status();
        return 0;
    }
    if(step_assets() != 0) return 1;
    if(step_files() != 0) return 1;
    step_wifi();
    step_token();
    ui_ok("Setup " SETUP_VERSION " complete.\n\n"
          "The payload is in GoldHEN's bin/elf. To start it, open GoldHEN's\n"
          "payload menu and pick orbisrpc, or enable AutoRun for it once\n"
          "so it starts on every jailbreak.");
    return 0;
}
