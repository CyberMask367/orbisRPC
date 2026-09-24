/* send.c - loopback payload injection: file bytes over TCP to the
 * console's own loaders. Chunked send (no whole-file malloc). */
#include "send.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/time.h>

#define SEND_CHUNK (64u*1024u)
static const int SEND_PORTS[] = { 9090, 9021, 9020 };

static int send_one(int fd, const char *path, void (*progress)(unsigned)){
    FILE *f = fopen(path, "rb");
    unsigned char buf[SEND_CHUNK];
    long total = 0, sent = 0;
    size_t n;
    if(!f) return -1;
    if(fseek(f, 0, SEEK_END) != 0){ fclose(f); return -1; }
    total = ftell(f);
    if(total <= 0 || total > 16*1024*1024){ fclose(f); return -1; }
    if(fseek(f, 0, SEEK_SET) != 0){ fclose(f); return -1; }
    if(progress) progress(0);
    while((n = fread(buf, 1, sizeof buf, f)) > 0){
        size_t off = 0;
        while(off < n){
            ssize_t w = send(fd, buf + off, n - off, 0);
            if(w <= 0){
                if(errno == EAGAIN || errno == EWOULDBLOCK) continue;
                fclose(f);
                return -1;
            }
            off += (size_t)w;
        }
        sent += (long)n;
        if(progress) progress((unsigned)(sent * 100 / total));
    }
    fclose(f);
    if(progress) progress(100);
    return 0;
}

int send_file_loopback(const char *path, int *port_used, void (*progress)(unsigned)){
    unsigned i;
    if(!path || !path[0]) return -1;
    for(i = 0; i < sizeof SEND_PORTS/sizeof SEND_PORTS[0]; i++){
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        struct sockaddr_in sa;
        struct timeval tv = { 5, 0 };
        if(fd < 0) continue;
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        memset(&sa, 0, sizeof sa);
        sa.sin_family = AF_INET;
        sa.sin_port = htons((uint16_t)SEND_PORTS[i]);
        sa.sin_addr.s_addr = inet_addr("127.0.0.1");
        if(connect(fd, (struct sockaddr *)&sa, sizeof sa) < 0){
            close(fd);
            continue;
        }
        if(send_one(fd, path, progress) == 0){
            close(fd);
            if(port_used) *port_used = SEND_PORTS[i];
            return 0;
        }
        close(fd);
    }
    return -1;
}
