/* send.h - loopback payload injection (clean-room, standard sockets).
 * Tries native GoldHEN BinLoader 9090, then elfldr 9021, then 9020.
 * 9020 is one-shot: accepted bytes count as sent. */
#ifndef INSTALLER_SEND_H
#define INSTALLER_SEND_H

/* progress(pct) may be NULL. Returns 0 + port_used set on success. */
int send_file_loopback(const char *path, int *port_used, void (*progress)(unsigned pct));

#endif
