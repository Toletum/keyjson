#ifndef KEYJSON_HTTP_H
#define KEYJSON_HTTP_H

#include "kv.h"

int http_server_run(kv_store *store, const char *host, const char *port);

#endif
