/* installer.c - orbisRPC Setup, stripped to the bone:
 * install files -> wifi check -> token -> inject -> done.
 * One confirm up front; everything after flows forward. */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/stat.h>
#include <errno.h>
#include "ui.h"
#include "send.h"
#include "icfg.h"
#include "nettest.h"

#define DAEMON_ELF "/app0/assets/daemon.elf"
#define EVICT_ELF "/app0/assets/evict.elf"
#define EVICT_RESULT "/data/orbisRPC/evict.txt"
#define INST_DIR "/data/orbisRPC"
#define INST_BIN "/data/orbisRPC/orbisrpc.elf"

static long file_mtime(const char *p){
    struct stat st;
    return (stat(p, &st) == 0) ? (long)st.st_mtime : 0;
}

static int copy_file(const char *src, const char *dst){
    FILE *in, *out;
    static unsigned char buf[65536];
    size_t n;
    in = fopen(src, "rb");
    if(!in) return -1;
    out = fopen(dst, "wb");
    if(!out){ fclose(in); return -1; }
    while((n = fread(buf, 1, sizeof buf, in)) > 0){
        if(fwrite(buf, 1, n, out) != n){ fclose(in); fclose(out); return -1; }
    }
    fclose(in);
    if(fclose(out) != 0) return -1;
    return 0;
}

/* 1: files. 0 ok, -1 fatal (no point continuing without the binary). */
static int step_files(void){
    mkdir(INST_DIR, 0777);
    if(copy_file(DAEMON_ELF, INST_BIN) != 0){
        ui_ok("Install failed: could not write the daemon.\n\nStopping here.");
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

/* 4: inject (evict rotation so reinstalls take over). */
static void step_inject(void){
    int port = 0;
    char msg[160];
    if(ui_progress_open("Starting orbisRPC...") != 0) return;
    remove(EVICT_RESULT);
    if(send_file_loopback(EVICT_ELF, &port, NULL) == 0){
        int waited = 0;
        while(waited < 40){
            sleep(2);
            waited += 2;
            if(file_mtime(EVICT_RESULT) != 0) break;
        }
    }
    ui_progress_msg("Starting orbisRPC...");
    ui_progress_set(0);
    if(send_file_loopback(DAEMON_ELF, &port, ui_progress_set) == 0){
        ui_progress_close();
        snprintf(msg, sizeof msg, "orbisRPC is running (loader port %d).\n\nLaunch a game and watch Discord.", port);
        ui_ok(msg);
    } else {
        ui_progress_close();
        ui_ok("Start failed on every loader port.\n\nIs GoldHEN's BinLoader on? The files are installed, so just relaunch this app later.");
    }
}

int main(void){
    int q;
    if(ui_init() != 0) return 1;
    q = ui_confirm("Install orbisRPC?\n\nCopies the daemon, sets up config, checks WiFi, saves your token, and starts it.");
    if(q != 1) return 0;
    if(step_files() != 0) return 0;
    step_wifi();
    step_token();
    step_inject();
    return 0;
}
