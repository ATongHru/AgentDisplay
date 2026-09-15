#pragma once

#include <stddef.h>

/* 从 ws://host[:port][/ws] 解析 host 与 port（缺省 port=8000）。 */
void ws_url_split_host_port(const char *ws_url, char *host, size_t host_len, int *port);

/* 拼接连接 URI：base URL + 可选 ?token=（忽略 base 上已有 query）。 */
void ws_url_build_connect_uri(const char *ws_url, const char *token, char *out, size_t out_len);
