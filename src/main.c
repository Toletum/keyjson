#define _POSIX_C_SOURCE 200809L

#include "http.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>

#define DEFAULT_HOST "127.0.0.1"
#define DEFAULT_PORT "8080"

static bool valid_port(const char *text) {
    char *end = NULL;
    unsigned long port;

    errno = 0;
    port = strtoul(text, &end, 10);
    return errno == 0 && end != text && *end == '\0' &&
           port >= 1UL && port <= 65535UL;
}

int main(int argc, char **argv) {
    const char *host = DEFAULT_HOST;
    const char *port = DEFAULT_PORT;
    kv_store *store;
    int result;

    if (argc > 3) {
        fprintf(stderr, "Usage: %s [host] [port]\n", argv[0]);
        return 2;
    }
    if (argc >= 2) {
        host = argv[1];
    }
    if (argc >= 3) {
        port = argv[2];
    }
    if (*host == '\0' || !valid_port(port)) {
        fprintf(stderr, "Host must be non-empty and port must be 1..65535\n");
        return 2;
    }

    store = kv_store_create();
    if (store == NULL) {
        fprintf(stderr, "Unable to create the key-value store\n");
        return 1;
    }
    result = http_server_run(store, host, port);
    kv_store_destroy(store);
    return result == 0 ? 0 : 1;
}
