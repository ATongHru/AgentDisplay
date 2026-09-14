#include "ws_url.h"

#include <stdlib.h>
#include <string.h>

void ws_url_split_host_port(const char *ws_url, char *host, size_t host_len, int *port)
{
    if (!host || host_len == 0 || !port) {
        return;
    }
    host[0] = 0;
    *port = 8000;
    if (!ws_url) {
        return;
    }
    const char *u = ws_url;
    if (strncmp(u, "ws://", 5) == 0) {
        u += 5;
    } else if (strncmp(u, "wss://", 6) == 0) {
        u += 6;
    }
    char tmp[128];
    size_t n = strlen(u);
    if (n >= 3 && strcmp(u + n - 3, "/ws") == 0) {
        n -= 3;
    }
    if (n >= sizeof(tmp)) {
        n = sizeof(tmp) - 1;
    }
    memcpy(tmp, u, n);
    tmp[n] = 0;
    char *colon = strrchr(tmp, ':');
    if (colon) {
        *colon = 0;
        int p = atoi(colon + 1);
        if (p > 0 && p <= 65535) {
            *port = p;
        }
    }
    strncpy(host, tmp, host_len - 1);
    host[host_len - 1] = 0;
}
