/* daemon.c - orbisRPC core loop, usable from the ELF payload (main.c) or from a
 * GoldHEN plugin. When `fixed_game_name` is non-NULL the daemon posts presence
 * for that game unconditionally (the plugin runs inside the game process and
 * already knows the title); otherwise it detects the foreground game.
 *
 * Auth model: a Discord USER SESSION token pasted into config.json ("token").
 * There is no OAuth flow anymore — OAuth2 access tokens are rejected by the
 * gateway (close 4004), which was why v1 never worked.
 *
 * A `stop` flag is polled so plugin_unload() can shut the loop down cleanly.
 *
 * KNOWN GAP: no suspend/resume notification handling. After Rest Mode the
 * sockets are dead; the heartbeat ack timeout plus reconnect loop should
 * recover, but suspend behavior is UNVERIFIED on hardware.
 */
#include "cfg.h"
#include "clock.h"
#include "lock.h"
#include "timesync.h"
#include "log.h"
#include "ws.h"
#include "discord.h"
#include "detect.h"
#include "updater.h"
#include "version.h"
#include <sys/stat.h>
#include <unistd.h>
#include <string.h>
#include <time.h>

extern cfg_t g_cfg;

static volatile int s_stop = 0;

/* Tells a running daemon_run() to shut down (called from plugin_unload). */
void daemon_request_stop(void){ s_stop = 1; }
void daemon_clear_stop(void){ s_stop = 0; }
int daemon_stop_requested(void){ return s_stop; }

/* Sleep up to `secs` but wake within a second when the plugin asks us to
 * stop, so plugin_unload() never hangs on a long backoff. Returns 1 stopped. */
static int sleep_stop(int secs){
    for(int i = 0; i < secs; i++){
        if(s_stop) return 1;
        sleep(1);
    }
    return s_stop;
}

static int have_token(const cfg_t *c){
    if(!c || !c->token[0]) return 0;
    if(strcmp(c->token,"SET_ME")==0) return 0;
    /* reject whitespace-only / trivially short tokens */
    size_t n = strlen(c->token);
    if(n < 16) return 0;
    for(size_t i = 0; i < n; i++){
        if(c->token[i]!=' ' && c->token[i]!='\t' && c->token[i]!='\r' && c->token[i]!='\n')
            return 1;
    }
    return 0;
}

/* fixed_game_name != NULL -> post presence for that game only, no detection.
 * NULL -> poll the foreground app like the payload daemon does.
 * Returns 0 normal stop, 1 config error, 2 auth-fatal (bad token). */
int daemon_run(const char *fixed_game_name){
    s_stop = 0;
    if(mkdir(DATA_DIR, 0777) != 0){
        /* EEXIST is fine; anything else means payload can't persist config/log */
        struct stat st;
        if(stat(DATA_DIR, &st) != 0){
            log_init(LOG_PATH);
            log_msg("FATAL: cannot create %s; check /data writable", DATA_DIR);
            log_close();
            return 1;
        }
    }
    log_init(LOG_PATH);
    log_msg("orbisRPC daemon start — build %s %s", __DATE__, __TIME__);
    /* Single writer: a second launch (or a stale pileup from repeated
     * injections) stands down instead of fighting over the gateway. */
    {
        int lr = lock_acquire();
        if(lr == 1){ log_msg("another daemon holds the lock; standing down"); log_close(); return 0; }
        if(lr != 0) log_msg("WARN: lock error; continuing without guard");
    }
    if(cfg_load(CFG_PATH, &g_cfg) != 0){
        /* First boot: drop a template so FTP edit is the only step */
        FILE *probe = fopen(CFG_PATH, "rb");
        if(!probe){
            cfg_defaults(&g_cfg);
            cfg_save(CFG_PATH, &g_cfg);
            log_msg("created template %s; edit \"token\" over FTP then reboot", CFG_PATH);
        } else fclose(probe);
        log_msg("config load failed; running on defaults until valid config appears");
    }
    if(!g_cfg.enabled){ log_msg("disabled in config; exiting"); log_close(); return 0; }
    log_set_debug(g_cfg.debug);
    if(g_cfg.debug) log_dbg("debug logging on (config)");
    /* Wall-clock correction for Discord timestamps (PSN time sync is
     * typically blocked on jailbroken consoles). Non-fatal. */
    time_sync();
    if(!have_token(&g_cfg)){
        log_msg("FATAL: put your Discord user token in %s as \"token\":\"...\"", CFG_PATH);
        log_close();
        return 1;
    }

    /* Self-update once per boot, before first connect. Never fatal:
     * staged artifacts take effect on next launch/injection. */
    if(g_cfg.auto_update){
        int ur = updater_check_and_stage();
        log_msg("updater: %s (local %s)",
                ur > 0 ? "staged newer build" : ur == 0 ? "already current" : "check failed",
                ORBISRPC_VERSION);
    }

    discord_t dc;
    int base_poll = g_cfg.poll_interval_s;
    if(base_poll < 5) base_poll = 5;
    if(base_poll > 60) base_poll = 60;
    int backoff = base_poll;
    for(;;){ /* outer: connect cycles with backoff on failure */
        if(s_stop) break;
        /* re-read config every cycle so token edits land without a reboot.
         * On parse failure keep last-good config instead of stale defaults. */
        {
            cfg_t next = g_cfg;
            if(cfg_load(CFG_PATH, &next) == 0){
                if(next.debug != g_cfg.debug)
                    log_msg("debug logging %s", next.debug ? "on" : "off");
                g_cfg = next;
                log_set_debug(g_cfg.debug);
            }
            else log_msg("config reload failed; keeping last-good config");
        }
        if(!have_token(&g_cfg)){
            log_msg("no token in config; waiting %ds", backoff);
            if(sleep_stop(backoff)) break;
            continue;
        }
        int rc = discord_connect(&dc, g_cfg.token);
        if(rc == -2){
            log_msg("FATAL: token rejected by gateway (close 4004). "
                    "Fix \"token\" in %s", CFG_PATH);
            return 2;
        }
        if(rc != 0){
            log_msg("gateway connect failed; retry in %ds", backoff);
            if(sleep_stop(backoff)) break;
            backoff *= 2; if(backoff > 300) backoff = 300;
            continue;
        }
        backoff = g_cfg.poll_interval_s; /* success resets backoff */
        if(backoff < 5) backoff = 5;
        if(backoff > 60) backoff = 60;

        /* last/started live OUTSIDE the session loop so the playing timer
         * survives gateway reconnects (same game keeps its original start).
         * A reconnect re-posts presence only when needed, timer intact. */
        static int active = 0;
        static char last[128] = "";
        static char sess_tid[16] = "";
        static int64_t started = 0;
        static int64_t last_tsync = 0;
        /* Resume window: if the same title vanishes briefly (detection
         * flicker, quick menu hop) and returns within 10 minutes, the
         * timer resumes instead of resetting to 0:00. */
        static char prev_tid[16] = "";
        static int64_t prev_started = 0, prev_end = 0;
        /* Transition debounce: shell flickers during launches, so a new
         * title (or disappearance) needs consecutive polls (2 at ~1s). */
        static char cand_title[16] = "";
        static int cand_hits = 0, miss_hits = 0;
        int64_t last_poll = 0;
        int64_t last_alive = 0;
        static int home_posted = 0;
        /* re-post after every (re)connect so Discord never sticks on stale */
        int need_post = active && last[0];
        while(!s_stop){ /* inner: live session, serviced every second */
            int64_t now = orbis_mono_s();
            if(now - last_tsync >= 3600){ last_tsync = now; time_sync(); }
            if(now != last_poll){
                /* Sleep/wake detection without a suspend API: if the loop
                 * itself froze (Rest Mode), mono jumps and heartbeats died.
                 * Log it and force a re-post so presence resumes cleanly. */
                if(last_poll != 0 && now - last_poll > 120){
                    log_msg("wake: loop was frozen %llds (likely Rest Mode); resuming",
                            (long long)(now - last_poll));
                    need_post = active && last[0];
                }
                last_poll = now;
                if(now - last_alive >= 60){
                    last_alive = now;
                    log_msg("alive: %s", active ? last : "idle");
                }
                char name[128] = "";
                if(fixed_game_name){
                    strncpy(name, fixed_game_name, sizeof name-1);
                    name[sizeof name-1] = 0;
                }else{
                    /* detect_current_game already checks foreground-active
                     * internally; don't double-call it. */
                    if(detect_current_game(name, sizeof name, NULL, 0) != 0)
                        name[0] = 0;
                }

                if(name[0]){
                    const char *cur_tid = detect_last_titleid();
                    if(!cur_tid) cur_tid = "";
                    if(!strncmp(cur_tid, cand_title, sizeof cand_title)){
                        cand_hits++;
                    } else {
                        strncpy(cand_title, cur_tid, sizeof cand_title-1);
                        cand_title[sizeof cand_title-1] = 0;
                        cand_hits = 1;
                    }
                    miss_hits = 0;
                    if(fixed_game_name || cand_hits >= 2){
                    if(!active || strncmp(cur_tid,sess_tid,sizeof sess_tid)!=0){
                        active = 1;
                        strncpy(sess_tid, cur_tid, sizeof sess_tid-1);
                        strncpy(last, name, sizeof last-1);
                        last[sizeof last-1] = 0;
                        if(!strncmp(cur_tid, prev_tid, sizeof prev_tid) &&
                           prev_started > 0 &&
                           now - prev_end < 600){
                            started = prev_started;
                            log_msg("SESSION_RESUMED title=%s (gap %llds, timer kept)",
                                    cur_tid[0]?cur_tid:"?", (long long)(now - prev_end));
                        } else {
                            started = time_fixed();
                        }
                        need_post = 1;
                        log_msg("GAME_DETECTED title=%s name=%s", cur_tid[0]?cur_tid:"?", name);
                    } else if(strncmp(name,last,sizeof last)!=0){
                        strncpy(last, name, sizeof last-1);
                        last[sizeof last-1] = 0;
                        need_post = 1;
                    }
                    if(need_post){
                        const char *state = g_cfg.presence_state[0] ? g_cfg.presence_state : NULL;
                        const char *tart = detect_last_art();
                        discord_set_presence_ex(&dc, state, last, sess_tid[0]?sess_tid:NULL, g_cfg.application_id, g_cfg.art_base_url, tart, started);
                        log_msg("presence: %s", last);
                        active = 1; need_post = 0;
                    }
                    }
                }else if(active){
                    cand_title[0] = 0;
                    cand_hits = 0;
                    if(++miss_hits >= 2){
                        discord_clear_presence(&dc);
                        log_msg("presence cleared");
                        strncpy(prev_tid, sess_tid, sizeof prev_tid-1);
                        prev_started = started;
                        prev_end = now;
                        last[0]=0; sess_tid[0]=0; active=0; started=0;
                        home_posted = 0;
                    }
                } else if(!home_posted){
                    /* Home screen support: no game running. Post a timerless
                     * home presence once (instead of bare online), clear it
                     * the moment a game commits. */
                    const char *state = g_cfg.presence_state[0] ? g_cfg.presence_state : NULL;
                    discord_set_presence_ex(&dc, state, "PlayStation 4", NULL,
                                            g_cfg.application_id, NULL, NULL, 0);
                    log_msg("presence: home");
                    home_posted = 1;
                }
            }

            /* service the gateway every pass (~1s): heartbeats must never be
             * more than a second or two late or the server drops us */
            int tr = discord_tick(&dc);
            if(tr == -2){
                log_msg("FATAL: token rejected (close 4004). Fix %s", CFG_PATH);
                ws_close(&dc.ws);
                log_close();
                return 2;
            }
            if(tr == -3){ log_msg("invalid session; fresh identify"); backoff = base_poll; ws_close(&dc.ws); break; }
            if(tr != 0){ log_msg("gateway dropped; reconnecting"); break; }

            for(int i = 0; i < 10 && !s_stop; i++) usleep(100000);
        }
        if(s_stop && dc.connected) discord_clear_presence(&dc);
        ws_close(&dc.ws);
    }
    log_close();
    return 0;
}
