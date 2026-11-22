#define _POSIX_C_SOURCE 200809L

#include "http.h"

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "yyjson.h"

#define HTTP_MAX_CONNECTIONS 64U
#define HTTP_MAX_HEADER_SIZE (16U * 1024U)
#define HTTP_MAX_BODY_SIZE (1024U * 1024U)
#define HTTP_READ_CHUNK_SIZE 8192U
#define HTTP_MAX_INPUT_SIZE \
    (HTTP_MAX_HEADER_SIZE + HTTP_MAX_BODY_SIZE + HTTP_READ_CHUNK_SIZE)
#define HTTP_MAX_KEY_SIZE 1024U
#define HTTP_IDLE_TIMEOUT_MS UINT64_C(30000)
#define HTTP_REQUEST_TIMEOUT_MS UINT64_C(30000)
#define HTTP_POLL_TIMEOUT_MS 1000

static volatile sig_atomic_t server_stopping;

typedef struct {
    int fd;
    char *input;
    size_t input_len;
    size_t input_capacity;
    char *output;
    size_t output_len;
    size_t output_offset;
    bool close_after_write;
    bool request_in_progress;
    uint64_t last_activity_ms;
    uint64_t request_started_ms;
} http_client;

typedef struct {
    const char *method;
    size_t method_len;
    const char *target;
    size_t target_len;
    size_t header_len;
    size_t content_length;
    bool keep_alive;
    bool content_type_present;
    bool content_type_json;
    bool ttl_present;
    uint64_t ttl_seconds;
} http_request;

typedef enum {
    REQUEST_NEED_MORE = 0,
    REQUEST_READY,
    REQUEST_BAD,
    REQUEST_HEADER_TOO_LARGE,
    REQUEST_BODY_TOO_LARGE,
    REQUEST_VERSION_UNSUPPORTED,
    REQUEST_EXPECTATION_FAILED
} request_parse_result;

typedef struct {
    yyjson_mut_doc *doc;
    yyjson_mut_val *array;
    bool failed;
} key_list_context;

static void request_shutdown(int signal_number) {
    (void)signal_number;
    server_stopping = 1;
}

static uint64_t monotonic_millis(void) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        return 0;
    }
    return (uint64_t)now.tv_sec * UINT64_C(1000) +
           (uint64_t)now.tv_nsec / UINT64_C(1000000);
}

static bool set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        return false;
    }
    return true;
}

static int create_listener(const char *host, const char *port) {
    struct addrinfo hints;
    struct addrinfo *addresses = NULL;
    struct addrinfo *address;
    int listener = -1;
    int result;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;

    result = getaddrinfo(host, port, &hints, &addresses);
    if (result != 0) {
        fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(result));
        return -1;
    }

    for (address = addresses; address != NULL; address = address->ai_next) {
        int reuse_address = 1;
        listener = socket(address->ai_family, address->ai_socktype,
                          address->ai_protocol);
        if (listener < 0) {
            continue;
        }
        if (setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &reuse_address,
                       sizeof(reuse_address)) < 0 ||
            !set_nonblocking(listener) ||
            bind(listener, address->ai_addr, address->ai_addrlen) < 0 ||
            listen(listener, (int)HTTP_MAX_CONNECTIONS) < 0) {
            close(listener);
            listener = -1;
            continue;
        }
        break;
    }

    freeaddrinfo(addresses);
    if (listener < 0) {
        fprintf(stderr, "Unable to bind %s:%s: %s\n", host, port,
                strerror(errno));
    }
    return listener;
}

static void close_client(http_client *client) {
    if (client->fd >= 0) {
        close(client->fd);
    }
    free(client->input);
    free(client->output);
    memset(client, 0, sizeof(*client));
    client->fd = -1;
}

static bool span_equal(const char *data, size_t length, const char *text) {
    size_t text_len = strlen(text);
    return length == text_len && memcmp(data, text, length) == 0;
}

static unsigned char ascii_lower(unsigned char value) {
    if (value >= (unsigned char)'A' && value <= (unsigned char)'Z') {
        return (unsigned char)(value + ((unsigned char)'a' - (unsigned char)'A'));
    }
    return value;
}

static bool span_case_equal(const char *data, size_t length, const char *text) {
    size_t text_len = strlen(text);
    if (length != text_len) {
        return false;
    }
    for (size_t i = 0; i < length; i++) {
        if (ascii_lower((unsigned char)data[i]) !=
            ascii_lower((unsigned char)text[i])) {
            return false;
        }
    }
    return true;
}

static bool is_token_character(unsigned char value) {
    if ((value >= (unsigned char)'0' && value <= (unsigned char)'9') ||
        (value >= (unsigned char)'A' && value <= (unsigned char)'Z') ||
        (value >= (unsigned char)'a' && value <= (unsigned char)'z')) {
        return true;
    }
    switch (value) {
        case '!':
        case '#':
        case '$':
        case '%':
        case '&':
        case '\'':
        case '*':
        case '+':
        case '-':
        case '.':
        case '^':
        case '_':
        case '`':
        case '|':
        case '~':
            return true;
        default:
            return false;
    }
}

static size_t find_crlf(const char *data, size_t start, size_t end) {
    if (end < 2U) {
        return SIZE_MAX;
    }
    for (size_t i = start; i + 1U < end; i++) {
        if (data[i] == '\r' && data[i + 1U] == '\n') {
            return i;
        }
    }
    return SIZE_MAX;
}

static size_t find_header_separator(const char *data, size_t length) {
    if (length < 4U) {
        return SIZE_MAX;
    }
    for (size_t i = 0; i <= length - 4U; i++) {
        if (data[i] == '\r' && data[i + 1U] == '\n' &&
            data[i + 2U] == '\r' && data[i + 3U] == '\n') {
            return i;
        }
    }
    return SIZE_MAX;
}

static void trim_ows(const char **value, size_t *length) {
    while (*length > 0U && (**value == ' ' || **value == '\t')) {
        (*value)++;
        (*length)--;
    }
    while (*length > 0U &&
           ((*value)[*length - 1U] == ' ' ||
            (*value)[*length - 1U] == '\t')) {
        (*length)--;
    }
}

static bool parse_decimal_size(const char *data, size_t length, size_t *value) {
    size_t parsed = 0;
    if (length == 0U) {
        return false;
    }
    for (size_t i = 0; i < length; i++) {
        unsigned char digit = (unsigned char)data[i];
        if (digit < (unsigned char)'0' || digit > (unsigned char)'9') {
            return false;
        }
        digit = (unsigned char)(digit - (unsigned char)'0');
        if (parsed > (SIZE_MAX - (size_t)digit) / 10U) {
            return false;
        }
        parsed = parsed * 10U + (size_t)digit;
    }
    *value = parsed;
    return true;
}

static bool parse_decimal_u64(const char *data,
                              size_t length,
                              uint64_t *value) {
    uint64_t parsed = 0U;
    if (length == 0U) {
        return false;
    }
    for (size_t i = 0U; i < length; i++) {
        unsigned char digit = (unsigned char)data[i];
        if (digit < (unsigned char)'0' || digit > (unsigned char)'9') {
            return false;
        }
        digit = (unsigned char)(digit - (unsigned char)'0');
        if (parsed > (UINT64_MAX - (uint64_t)digit) / UINT64_C(10)) {
            return false;
        }
        parsed = parsed * UINT64_C(10) + (uint64_t)digit;
    }
    *value = parsed;
    return true;
}

static bool is_json_content_type(const char *value, size_t length) {
    const char *semicolon = memchr(value, ';', length);
    if (semicolon != NULL) {
        length = (size_t)(semicolon - value);
    }
    trim_ows(&value, &length);
    return span_case_equal(value, length, "application/json");
}

static void parse_connection_tokens(const char *value,
                                   size_t length,
                                   bool *close_connection) {
    size_t start = 0;
    while (start < length) {
        size_t end = start;
        while (end < length && value[end] != ',') {
            end++;
        }
        const char *token = value + start;
        size_t token_len = end - start;
        trim_ows(&token, &token_len);
        if (span_case_equal(token, token_len, "close")) {
            *close_connection = true;
        }
        start = end + 1U;
    }
}

static request_parse_result parse_request(const char *data,
                                          size_t length,
                                          http_request *request) {
    size_t separator = find_header_separator(data, length);
    size_t line_end;
    size_t first_space;
    size_t second_space;
    size_t position;
    bool content_length_seen = false;
    bool content_type_seen = false;
    bool ttl_seen = false;
    bool host_seen = false;
    bool transfer_encoding_seen = false;
    bool expectation_seen = false;
    bool connection_close = false;

    memset(request, 0, sizeof(*request));
    if (separator == SIZE_MAX) {
        return length > HTTP_MAX_HEADER_SIZE ? REQUEST_HEADER_TOO_LARGE
                                             : REQUEST_NEED_MORE;
    }
    if (separator + 4U > HTTP_MAX_HEADER_SIZE) {
        return REQUEST_HEADER_TOO_LARGE;
    }
    request->header_len = separator + 4U;

    line_end = find_crlf(data, 0, separator + 2U);
    if (line_end == SIZE_MAX || line_end == 0U) {
        return REQUEST_BAD;
    }
    first_space = 0;
    while (first_space < line_end && data[first_space] != ' ') {
        if (!is_token_character((unsigned char)data[first_space])) {
            return REQUEST_BAD;
        }
        first_space++;
    }
    if (first_space == 0U || first_space == line_end) {
        return REQUEST_BAD;
    }
    second_space = first_space + 1U;
    while (second_space < line_end && data[second_space] != ' ') {
        unsigned char value = (unsigned char)data[second_space];
        if (value <= 0x20U || value == 0x7fU || value == '#') {
            return REQUEST_BAD;
        }
        second_space++;
    }
    if (second_space == first_space + 1U || second_space == line_end) {
        return REQUEST_BAD;
    }
    for (size_t i = second_space + 1U; i < line_end; i++) {
        if (data[i] == ' ') {
            return REQUEST_BAD;
        }
    }
    request->method = data;
    request->method_len = first_space;
    request->target = data + first_space + 1U;
    request->target_len = second_space - first_space - 1U;
    if (request->target[0] != '/') {
        return REQUEST_BAD;
    }
    if (!span_equal(data + second_space + 1U,
                    line_end - second_space - 1U, "HTTP/1.1")) {
        return REQUEST_VERSION_UNSUPPORTED;
    }

    position = line_end + 2U;
    while (position < separator) {
        size_t header_end = find_crlf(data, position, separator + 2U);
        size_t colon = position;
        const char *value;
        size_t value_len;

        if (header_end == SIZE_MAX || header_end == position) {
            return REQUEST_BAD;
        }
        while (colon < header_end && data[colon] != ':') {
            if (!is_token_character((unsigned char)data[colon])) {
                return REQUEST_BAD;
            }
            colon++;
        }
        if (colon == position || colon == header_end) {
            return REQUEST_BAD;
        }
        value = data + colon + 1U;
        value_len = header_end - colon - 1U;
        trim_ows(&value, &value_len);
        for (size_t i = 0; i < value_len; i++) {
            unsigned char character = (unsigned char)value[i];
            if ((character < 0x20U && character != '\t') ||
                character == 0x7fU) {
                return REQUEST_BAD;
            }
        }

        size_t name_len = colon - position;
        const char *name = data + position;
        if (span_case_equal(name, name_len, "host")) {
            if (host_seen || value_len == 0U) {
                return REQUEST_BAD;
            }
            host_seen = true;
        } else if (span_case_equal(name, name_len, "content-length")) {
            if (content_length_seen ||
                !parse_decimal_size(value, value_len,
                                    &request->content_length)) {
                return REQUEST_BAD;
            }
            content_length_seen = true;
            if (request->content_length > HTTP_MAX_BODY_SIZE) {
                return REQUEST_BODY_TOO_LARGE;
            }
        } else if (span_case_equal(name, name_len, "content-type")) {
            if (content_type_seen) {
                return REQUEST_BAD;
            }
            content_type_seen = true;
            request->content_type_present = true;
            request->content_type_json = is_json_content_type(value, value_len);
        } else if (span_case_equal(name, name_len, "x-ttl-seconds")) {
            if (ttl_seen ||
                !parse_decimal_u64(value, value_len, &request->ttl_seconds) ||
                request->ttl_seconds == 0U) {
                return REQUEST_BAD;
            }
            ttl_seen = true;
            request->ttl_present = true;
        } else if (span_case_equal(name, name_len, "transfer-encoding")) {
            transfer_encoding_seen = true;
        } else if (span_case_equal(name, name_len, "connection")) {
            parse_connection_tokens(value, value_len, &connection_close);
        } else if (span_case_equal(name, name_len, "expect")) {
            expectation_seen = true;
        }

        position = header_end + 2U;
    }

    if (!host_seen || transfer_encoding_seen) {
        return REQUEST_BAD;
    }
    if (expectation_seen) {
        return REQUEST_EXPECTATION_FAILED;
    }
    request->keep_alive = !connection_close;
    return REQUEST_READY;
}

static const char *status_reason(int status) {
    switch (status) {
        case 200:
            return "OK";
        case 201:
            return "Created";
        case 400:
            return "Bad Request";
        case 404:
            return "Not Found";
        case 405:
            return "Method Not Allowed";
        case 413:
            return "Payload Too Large";
        case 415:
            return "Unsupported Media Type";
        case 417:
            return "Expectation Failed";
        case 431:
            return "Request Header Fields Too Large";
        case 500:
            return "Internal Server Error";
        case 505:
            return "HTTP Version Not Supported";
        default:
            return "Internal Server Error";
    }
}

static bool queue_response(http_client *client,
                           int status,
                           const char *body,
                           size_t body_len,
                           bool keep_alive,
                           const char *allow) {
    char header[512];
    int header_len;
    if (client->output != NULL || (body_len > 0U && body == NULL)) {
        client->close_after_write = true;
        return false;
    }
    if (allow == NULL) {
        header_len = snprintf(header, sizeof(header),
                              "HTTP/1.1 %d %s\r\n"
                              "Content-Type: application/json; charset=utf-8\r\n"
                              "Content-Length: %zu\r\n"
                              "Connection: %s\r\n\r\n",
                              status, status_reason(status), body_len,
                              keep_alive ? "keep-alive" : "close");
    } else {
        header_len = snprintf(header, sizeof(header),
                              "HTTP/1.1 %d %s\r\n"
                              "Content-Type: application/json; charset=utf-8\r\n"
                              "Content-Length: %zu\r\n"
                              "Connection: %s\r\n"
                              "Allow: %s\r\n\r\n",
                              status, status_reason(status), body_len,
                              keep_alive ? "keep-alive" : "close", allow);
    }
    if (header_len < 0 || (size_t)header_len >= sizeof(header) ||
        (size_t)header_len > SIZE_MAX - body_len) {
        client->close_after_write = true;
        return false;
    }

    size_t total_len = (size_t)header_len + body_len;
    char *output = malloc(total_len == 0U ? 1U : total_len);
    if (output == NULL) {
        client->close_after_write = true;
        return false;
    }
    memcpy(output, header, (size_t)header_len);
    if (body_len > 0U) {
        memcpy(output + (size_t)header_len, body, body_len);
    }

    client->output = output;
    client->output_len = total_len;
    client->output_offset = 0;
    client->close_after_write = !keep_alive;
    client->last_activity_ms = monotonic_millis();
    return true;
}

static char *serialize_mutable(yyjson_mut_doc *doc,
                               yyjson_mut_val *root,
                               size_t *json_len) {
    char *json;
    if (doc == NULL || root == NULL) {
        yyjson_mut_doc_free(doc);
        return NULL;
    }
    yyjson_mut_doc_set_root(doc, root);
    json = yyjson_mut_write(doc, YYJSON_WRITE_NOFLAG, json_len);
    yyjson_mut_doc_free(doc);
    return json;
}

static char *make_error_json(const char *error, size_t *json_len) {
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *object;
    if (doc == NULL) {
        return NULL;
    }
    object = yyjson_mut_obj(doc);
    if (object == NULL ||
        !yyjson_mut_obj_add_strcpy(doc, object, "error", error)) {
        yyjson_mut_doc_free(doc);
        return NULL;
    }
    return serialize_mutable(doc, object, json_len);
}

static char *make_bool_json(const char *name, bool value, size_t *json_len) {
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *object;
    if (doc == NULL) {
        return NULL;
    }
    object = yyjson_mut_obj(doc);
    if (object == NULL || !yyjson_mut_obj_add_bool(doc, object, name, value)) {
        yyjson_mut_doc_free(doc);
        return NULL;
    }
    return serialize_mutable(doc, object, json_len);
}

static void respond_error(http_client *client,
                          int status,
                          const char *error,
                          bool keep_alive,
                          const char *allow) {
    static const char fallback[] = "{\"error\":\"internal_error\"}";
    size_t body_len = 0;
    char *body = make_error_json(error, &body_len);

    if (body == NULL) {
        (void)queue_response(client, status, fallback, sizeof(fallback) - 1U,
                             false, allow);
        return;
    }
    (void)queue_response(client, status, body, body_len, keep_alive, allow);
    free(body);
}

static bool append_key(const char *key, size_t key_len, void *context) {
    key_list_context *list = context;
    yyjson_mut_val *value = yyjson_mut_strncpy(list->doc, key, key_len);
    if (value == NULL || !yyjson_mut_arr_append(list->array, value)) {
        list->failed = true;
        return false;
    }
    return true;
}

static char *make_keys_json(kv_store *store, size_t *json_len) {
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *array;
    key_list_context context;

    if (doc == NULL) {
        return NULL;
    }
    array = yyjson_mut_arr(doc);
    if (array == NULL) {
        yyjson_mut_doc_free(doc);
        return NULL;
    }
    context.doc = doc;
    context.array = array;
    context.failed = false;
    if (!kv_store_visit(store, append_key, &context) || context.failed) {
        yyjson_mut_doc_free(doc);
        return NULL;
    }
    return serialize_mutable(doc, array, json_len);
}

static char *make_stats_json(kv_store *store, size_t *json_len) {
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *object;
    kv_stats stats;

    if (doc == NULL) {
        return NULL;
    }
    kv_store_get_stats(store, &stats);
    object = yyjson_mut_obj(doc);
    if (object == NULL ||
        !yyjson_mut_obj_add_uint(doc, object, "keys", (uint64_t)stats.key_count) ||
        !yyjson_mut_obj_add_uint(doc, object, "capacity", (uint64_t)stats.capacity) ||
        !yyjson_mut_obj_add_uint(doc, object, "stored_bytes",
                                 (uint64_t)stats.stored_bytes) ||
        !yyjson_mut_obj_add_uint(doc, object, "sets", stats.set_count) ||
        !yyjson_mut_obj_add_uint(doc, object, "gets", stats.get_count) ||
        !yyjson_mut_obj_add_uint(doc, object, "deletes", stats.delete_count)) {
        yyjson_mut_doc_free(doc);
        return NULL;
    }
    return serialize_mutable(doc, object, json_len);
}

static bool valid_utf8(const unsigned char *data, size_t length) {
    size_t i = 0;
    while (i < length) {
        unsigned char first = data[i];
        if (first <= 0x7fU) {
            i++;
        } else if (first >= 0xc2U && first <= 0xdfU) {
            if (i + 1U >= length || (data[i + 1U] & 0xc0U) != 0x80U) {
                return false;
            }
            i += 2U;
        } else if (first == 0xe0U) {
            if (i + 2U >= length || data[i + 1U] < 0xa0U ||
                data[i + 1U] > 0xbfU || (data[i + 2U] & 0xc0U) != 0x80U) {
                return false;
            }
            i += 3U;
        } else if ((first >= 0xe1U && first <= 0xecU) ||
                   (first >= 0xeeU && first <= 0xefU)) {
            if (i + 2U >= length || (data[i + 1U] & 0xc0U) != 0x80U ||
                (data[i + 2U] & 0xc0U) != 0x80U) {
                return false;
            }
            i += 3U;
        } else if (first == 0xedU) {
            if (i + 2U >= length || data[i + 1U] < 0x80U ||
                data[i + 1U] > 0x9fU || (data[i + 2U] & 0xc0U) != 0x80U) {
                return false;
            }
            i += 3U;
        } else if (first == 0xf0U) {
            if (i + 3U >= length || data[i + 1U] < 0x90U ||
                data[i + 1U] > 0xbfU || (data[i + 2U] & 0xc0U) != 0x80U ||
                (data[i + 3U] & 0xc0U) != 0x80U) {
                return false;
            }
            i += 4U;
        } else if (first >= 0xf1U && first <= 0xf3U) {
            if (i + 3U >= length || (data[i + 1U] & 0xc0U) != 0x80U ||
                (data[i + 2U] & 0xc0U) != 0x80U ||
                (data[i + 3U] & 0xc0U) != 0x80U) {
                return false;
            }
            i += 4U;
        } else if (first == 0xf4U) {
            if (i + 3U >= length || data[i + 1U] < 0x80U ||
                data[i + 1U] > 0x8fU || (data[i + 2U] & 0xc0U) != 0x80U ||
                (data[i + 3U] & 0xc0U) != 0x80U) {
                return false;
            }
            i += 4U;
        } else {
            return false;
        }
    }
    return true;
}

static int hex_value(unsigned char value) {
    if (value >= (unsigned char)'0' && value <= (unsigned char)'9') {
        return (int)(value - (unsigned char)'0');
    }
    if (value >= (unsigned char)'a' && value <= (unsigned char)'f') {
        return (int)(value - (unsigned char)'a') + 10;
    }
    if (value >= (unsigned char)'A' && value <= (unsigned char)'F') {
        return (int)(value - (unsigned char)'A') + 10;
    }
    return -1;
}

static bool decode_key(const char *encoded,
                       size_t encoded_len,
                       char *key,
                       size_t key_capacity,
                       size_t *key_len) {
    size_t source = 0;
    size_t destination = 0;

    while (source < encoded_len) {
        unsigned char value = (unsigned char)encoded[source];
        if (value == (unsigned char)'%') {
            int high;
            int low;
            if (encoded_len - source < 3U) {
                return false;
            }
            high = hex_value((unsigned char)encoded[source + 1U]);
            low = hex_value((unsigned char)encoded[source + 2U]);
            if (high < 0 || low < 0) {
                return false;
            }
            value = (unsigned char)((unsigned int)high * 16U + (unsigned int)low);
            source += 3U;
        } else {
            source++;
        }
        if (destination >= key_capacity || value == 0U || value < 0x20U ||
            value == 0x7fU) {
            return false;
        }
        key[destination++] = (char)value;
    }
    if (destination == 0U || !valid_utf8((const unsigned char *)key, destination)) {
        return false;
    }
    key[destination] = '\0';
    *key_len = destination;
    return true;
}

static bool is_put(const http_request *request) {
    return span_equal(request->method, request->method_len, "PUT");
}

static bool is_get(const http_request *request) {
    return span_equal(request->method, request->method_len, "GET");
}

static bool is_delete(const http_request *request) {
    return span_equal(request->method, request->method_len, "DELETE");
}

static void respond_method_not_allowed(http_client *client,
                                       const http_request *request,
                                       const char *allow) {
    respond_error(client, 405, "method_not_allowed", request->keep_alive,
                  allow);
}

static void handle_key_request(http_client *client,
                               kv_store *store,
                               const http_request *request,
                               const char *body) {
    char key[HTTP_MAX_KEY_SIZE + 1U];
    size_t key_len = 0;

    if (memchr(request->target, '?', request->target_len) != NULL) {
        respond_error(client, 400, "query_strings_not_supported",
                      request->keep_alive, NULL);
        return;
    }
    if (!decode_key(request->target + 6U, request->target_len - 6U,
                    key, HTTP_MAX_KEY_SIZE, &key_len)) {
        respond_error(client, 400, "invalid_key", request->keep_alive, NULL);
        return;
    }

    if (is_get(request)) {
        const char *value;
        size_t value_len;
        kv_status status = kv_store_get(store, key, key_len, &value, &value_len);
        if (status == KV_STATUS_NOT_FOUND) {
            respond_error(client, 404, "key_not_found", request->keep_alive,
                          NULL);
        } else if (status != KV_STATUS_OK) {
            respond_error(client, 500, "internal_error", false, NULL);
        } else {
            (void)queue_response(client, 200, value, value_len,
                                 request->keep_alive, NULL);
        }
        return;
    }

    if (is_put(request)) {
        yyjson_doc *document;
        yyjson_val *root;
        char *serialized;
        size_t serialized_len = 0;
        bool created = false;
        kv_status status;

        if (!request->content_type_present || !request->content_type_json) {
            respond_error(client, 415, "content_type_must_be_application_json",
                          request->keep_alive, NULL);
            return;
        }
        document = yyjson_read(body, request->content_length, YYJSON_READ_NOFLAG);
        if (document == NULL) {
            respond_error(client, 400, "invalid_json", request->keep_alive,
                          NULL);
            return;
        }
        root = yyjson_doc_get_root(document);
        serialized = root == NULL
                         ? NULL
                         : yyjson_val_write(root, YYJSON_WRITE_NOFLAG,
                                            &serialized_len);
        yyjson_doc_free(document);
        if (serialized == NULL) {
            respond_error(client, 500, "internal_error", false, NULL);
            return;
        }
        status = request->ttl_present
                     ? kv_store_set_with_ttl(store, key, key_len, serialized,
                                             serialized_len,
                                             request->ttl_seconds, &created)
                     : kv_store_set(store, key, key_len, serialized,
                                    serialized_len, &created);
        free(serialized);
        if (status == KV_STATUS_NO_MEMORY) {
            respond_error(client, 500, "out_of_memory", request->keep_alive,
                          NULL);
        } else if (status == KV_STATUS_INTERNAL_ERROR) {
            respond_error(client, 500, "internal_error", false, NULL);
        } else if (status != KV_STATUS_OK) {
            respond_error(client, 400, "invalid_request", request->keep_alive,
                          NULL);
        } else {
            size_t response_len = 0;
            char *response = make_bool_json("created", created, &response_len);
            if (response == NULL) {
                respond_error(client, 500, "out_of_memory", false, NULL);
                return;
            }
            (void)queue_response(client, created ? 201 : 200, response,
                                 response_len, request->keep_alive, NULL);
            free(response);
        }
        return;
    }

    if (is_delete(request)) {
        kv_status status = kv_store_delete(store, key, key_len);
        if (status == KV_STATUS_NOT_FOUND) {
            respond_error(client, 404, "key_not_found", request->keep_alive,
                          NULL);
        } else if (status != KV_STATUS_OK) {
            respond_error(client, 500, "internal_error", false, NULL);
        } else {
            size_t response_len = 0;
            char *response = make_bool_json("deleted", true, &response_len);
            if (response == NULL) {
                respond_error(client, 500, "out_of_memory", false, NULL);
                return;
            }
            (void)queue_response(client, 200, response, response_len,
                                 request->keep_alive, NULL);
            free(response);
        }
        return;
    }

    respond_method_not_allowed(client, request, "GET, PUT, DELETE");
}

static void handle_request(http_client *client,
                           kv_store *store,
                           const http_request *request,
                           const char *body) {
    if (request->ttl_present && !is_put(request)) {
        respond_error(client, 400, "ttl_only_supported_on_put",
                      request->keep_alive, NULL);
        return;
    }
    if (memchr(request->target, '?', request->target_len) != NULL) {
        respond_error(client, 400, "query_strings_not_supported",
                      request->keep_alive, NULL);
        return;
    }
    if (span_equal(request->target, request->target_len, "/keys")) {
        if (!is_get(request)) {
            respond_method_not_allowed(client, request, "GET");
            return;
        }
        size_t json_len = 0;
        char *json = make_keys_json(store, &json_len);
        if (json == NULL) {
            respond_error(client, 500, "out_of_memory", request->keep_alive,
                          NULL);
            return;
        }
        (void)queue_response(client, 200, json, json_len,
                             request->keep_alive, NULL);
        free(json);
        return;
    }
    if (span_equal(request->target, request->target_len, "/stats")) {
        if (!is_get(request)) {
            respond_method_not_allowed(client, request, "GET");
            return;
        }
        size_t json_len = 0;
        char *json = make_stats_json(store, &json_len);
        if (json == NULL) {
            respond_error(client, 500, "out_of_memory", request->keep_alive,
                          NULL);
            return;
        }
        (void)queue_response(client, 200, json, json_len,
                             request->keep_alive, NULL);
        free(json);
        return;
    }
    if (request->target_len >= 6U &&
        memcmp(request->target, "/keys/", 6U) == 0) {
        handle_key_request(client, store, request, body);
        return;
    }
    respond_error(client, 404, "route_not_found", request->keep_alive, NULL);
}

static void process_buffered_request(http_client *client, kv_store *store) {
    http_request request;
    request_parse_result result;
    size_t request_len;

    if (client->fd < 0 || client->output != NULL ||
        client->close_after_write || client->input_len == 0U) {
        return;
    }
    result = parse_request(client->input, client->input_len, &request);
    if (result == REQUEST_NEED_MORE) {
        return;
    }
    if (result == REQUEST_HEADER_TOO_LARGE) {
        respond_error(client, 431, "request_headers_too_large", false, NULL);
        return;
    }
    if (result == REQUEST_BODY_TOO_LARGE) {
        respond_error(client, 413, "request_body_too_large", false, NULL);
        return;
    }
    if (result == REQUEST_VERSION_UNSUPPORTED) {
        respond_error(client, 505, "http_version_not_supported", false, NULL);
        return;
    }
    if (result == REQUEST_EXPECTATION_FAILED) {
        respond_error(client, 417, "expectation_not_supported", false, NULL);
        return;
    }
    if (result != REQUEST_READY) {
        respond_error(client, 400, "bad_request", false, NULL);
        return;
    }
    if (request.content_length > SIZE_MAX - request.header_len) {
        respond_error(client, 413, "request_body_too_large", false, NULL);
        return;
    }
    request_len = request.header_len + request.content_length;
    if (client->input_len < request_len) {
        return;
    }

    handle_request(client, store, &request,
                   client->input + request.header_len);
    size_t remaining = client->input_len - request_len;
    if (remaining > 0U) {
        memmove(client->input, client->input + request_len, remaining);
    }
    client->input_len = remaining;
    client->request_in_progress = remaining > 0U;
    client->request_started_ms =
        client->request_in_progress ? monotonic_millis() : 0U;
}

static bool reserve_input(http_client *client, size_t needed) {
    size_t capacity;
    char *input;

    if (needed > HTTP_MAX_INPUT_SIZE) {
        return false;
    }
    if (client->input_capacity >= needed) {
        return true;
    }
    capacity = client->input_capacity == 0U ? 4096U : client->input_capacity;
    while (capacity < needed) {
        if (capacity > HTTP_MAX_INPUT_SIZE / 2U) {
            capacity = HTTP_MAX_INPUT_SIZE;
            break;
        }
        capacity *= 2U;
    }
    input = realloc(client->input, capacity);
    if (input == NULL) {
        return false;
    }
    client->input = input;
    client->input_capacity = capacity;
    return true;
}

static void read_client(http_client *client, kv_store *store) {
    size_t needed;
    size_t available;
    ssize_t received;

    if (client->input_len >= HTTP_MAX_INPUT_SIZE) {
        respond_error(client, 413, "request_too_large", false, NULL);
        return;
    }
    needed = client->input_len + HTTP_READ_CHUNK_SIZE;
    if (needed < client->input_len || needed > HTTP_MAX_INPUT_SIZE) {
        needed = HTTP_MAX_INPUT_SIZE;
    }
    if (!reserve_input(client, needed)) {
        respond_error(client, 500, "out_of_memory", false, NULL);
        return;
    }
    available = client->input_capacity - client->input_len;
    if (available > HTTP_READ_CHUNK_SIZE) {
        available = HTTP_READ_CHUNK_SIZE;
    }
    received = recv(client->fd, client->input + client->input_len, available, 0);
    if (received > 0) {
        uint64_t now = monotonic_millis();
        client->input_len += (size_t)received;
        client->last_activity_ms = now;
        if (!client->request_in_progress) {
            client->request_in_progress = true;
            client->request_started_ms = now;
        }
        process_buffered_request(client, store);
    } else if (received == 0) {
        close_client(client);
    } else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
        close_client(client);
    }
}

static void write_client(http_client *client) {
    size_t remaining = client->output_len - client->output_offset;
    ssize_t sent = send(client->fd, client->output + client->output_offset,
                        remaining, 0);

    if (sent > 0) {
        client->output_offset += (size_t)sent;
        client->last_activity_ms = monotonic_millis();
        if (client->output_offset == client->output_len) {
            free(client->output);
            client->output = NULL;
            client->output_len = 0;
            client->output_offset = 0;
            if (client->close_after_write) {
                close_client(client);
            } else if (client->request_in_progress) {
                client->request_started_ms = client->last_activity_ms;
            }
        }
    } else if (sent == 0 ||
               (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
        close_client(client);
    }
}

static void accept_clients(int listener,
                           http_client clients[HTTP_MAX_CONNECTIONS]) {
    for (;;) {
        int fd = accept(listener, NULL, NULL);
        size_t slot;

        if (fd < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                fprintf(stderr, "accept: %s\n", strerror(errno));
            }
            return;
        }
        if (!set_nonblocking(fd)) {
            close(fd);
            continue;
        }
        for (slot = 0; slot < HTTP_MAX_CONNECTIONS; slot++) {
            if (clients[slot].fd < 0) {
                break;
            }
        }
        if (slot == HTTP_MAX_CONNECTIONS) {
            close(fd);
            continue;
        }
        memset(&clients[slot], 0, sizeof(clients[slot]));
        clients[slot].fd = fd;
        clients[slot].last_activity_ms = monotonic_millis();
    }
}

int http_server_run(kv_store *store, const char *host, const char *port) {
    http_client clients[HTTP_MAX_CONNECTIONS];
    struct pollfd poll_fds[HTTP_MAX_CONNECTIONS + 1U];
    struct sigaction action;
    struct sigaction ignored_action;
    int listener;
    int result = 0;

    if (store == NULL || host == NULL || port == NULL) {
        return -1;
    }
    for (size_t i = 0; i < HTTP_MAX_CONNECTIONS; i++) {
        memset(&clients[i], 0, sizeof(clients[i]));
        clients[i].fd = -1;
    }

    memset(&action, 0, sizeof(action));
    action.sa_handler = request_shutdown;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGINT, &action, NULL) < 0 ||
        sigaction(SIGTERM, &action, NULL) < 0) {
        fprintf(stderr, "sigaction: %s\n", strerror(errno));
        return -1;
    }
    memset(&ignored_action, 0, sizeof(ignored_action));
    ignored_action.sa_handler = SIG_IGN;
    sigemptyset(&ignored_action.sa_mask);
    if (sigaction(SIGPIPE, &ignored_action, NULL) < 0) {
        fprintf(stderr, "sigaction: %s\n", strerror(errno));
        return -1;
    }

    server_stopping = 0;
    listener = create_listener(host, port);
    if (listener < 0) {
        return -1;
    }
    fprintf(stderr, "Listening on %s:%s\n", host, port);

    while (!server_stopping) {
        uint64_t now = monotonic_millis();
        int poll_result;

        for (size_t i = 0; i < HTTP_MAX_CONNECTIONS; i++) {
            http_client *client = &clients[i];
            if (client->fd < 0) {
                continue;
            }
            if ((client->output == NULL && client->request_in_progress &&
                 now >= client->request_started_ms &&
                 now - client->request_started_ms > HTTP_REQUEST_TIMEOUT_MS) ||
                (now >= client->last_activity_ms &&
                 now - client->last_activity_ms > HTTP_IDLE_TIMEOUT_MS)) {
                close_client(client);
                continue;
            }
            process_buffered_request(client, store);
            if (client->fd >= 0 && client->close_after_write &&
                client->output == NULL) {
                close_client(client);
            }
        }

        poll_fds[0].fd = listener;
        poll_fds[0].events = POLLIN;
        poll_fds[0].revents = 0;
        for (size_t i = 0; i < HTTP_MAX_CONNECTIONS; i++) {
            poll_fds[i + 1U].fd = clients[i].fd;
            poll_fds[i + 1U].events = clients[i].fd < 0
                                          ? 0
                                          : clients[i].output == NULL ? POLLIN
                                                                      : POLLOUT;
            poll_fds[i + 1U].revents = 0;
        }

        poll_result = poll(poll_fds, HTTP_MAX_CONNECTIONS + 1U,
                           HTTP_POLL_TIMEOUT_MS);
        if (poll_result < 0) {
            if (errno == EINTR) {
                continue;
            }
            fprintf(stderr, "poll: %s\n", strerror(errno));
            result = -1;
            break;
        }
        if (poll_fds[0].revents & POLLIN) {
            accept_clients(listener, clients);
        }
        if (poll_fds[0].revents & (POLLERR | POLLHUP | POLLNVAL)) {
            fprintf(stderr, "Listening socket failed\n");
            result = -1;
            break;
        }
        for (size_t i = 0; i < HTTP_MAX_CONNECTIONS; i++) {
            http_client *client = &clients[i];
            short events = poll_fds[i + 1U].revents;
            if (client->fd < 0 || events == 0) {
                continue;
            }
            if (events & (POLLERR | POLLNVAL)) {
                close_client(client);
                continue;
            }
            if (client->output == NULL && (events & (POLLIN | POLLHUP))) {
                read_client(client, store);
            } else if (client->output != NULL && (events & POLLOUT)) {
                write_client(client);
            } else if (events & POLLHUP) {
                close_client(client);
            }
        }
    }

    close(listener);
    for (size_t i = 0; i < HTTP_MAX_CONNECTIONS; i++) {
        close_client(&clients[i]);
    }
    return result;
}
