#pragma once

#include <stddef.h>

/* 从 ws://host[:port][/ws] 解析 host 与 port（缺省 port=8000）。 */
void ws_url_split_host_port(const char *ws_url, char *host, size_t host_len, int *port);
