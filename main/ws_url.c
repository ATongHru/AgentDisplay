#include "ws_url.h"

#include <stdio.h>
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
    const char *q = strchr(u, '?');
    if (q) {
        n = (size_t)(q - u);
    }
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

void ws_url_build_connect_uri(const char *ws_url, const char *token, char *out, size_t out_len)
{
    if (!out || out_len == 0) {
        return;
    }
    out[0] = 0;
    if (!ws_url || !ws_url[0]) {
        return;
    }
    char base[160];
    strncpy(base, ws_url, sizeof(base) - 1);
    base[sizeof(base) - 1] = 0;
    char *q = strchr(base, '?');
    if (q) {
        *q = 0;
    }
    if (token && token[0]) {
        snprintf(out, out_len, "%s?token=%s", base, token);
    } else {
        strncpy(out, base, out_len - 1);
        out[out_len - 1] = 0;
    }
}
