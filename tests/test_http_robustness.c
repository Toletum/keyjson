#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#define DEFAULT_TEST_PORT 8080U
#define RESPONSE_BUFFER_SIZE 16384U
#define STRESS_THREAD_COUNT 8U
#define STRESS_REQUESTS_PER_THREAD 100U

static const char stats_request[] =
    "GET /stats HTTP/1.1\r\n"
    "Host: localhost\r\n"
    "Connection: keep-alive\r\n\r\n";

typedef struct {
    int fd;
    char buffer[RESPONSE_BUFFER_SIZE];
    size_t length;
} test_client;


static size_t checks_run;
static size_t checks_failed;
static uint16_t test_port = (uint16_t)DEFAULT_TEST_PORT;

static bool parse_test_port(const char *text, uint16_t *port) {
    uint32_t parsed = 0U;
    if (text == NULL || *text == '\0') {
        return false;
    }
    for (const char *character = text; *character != '\0'; character++) {
        unsigned char digit = (unsigned char)*character;
        if (digit < (unsigned char)'0' || digit > (unsigned char)'9') {
            return false;
        }
        digit = (unsigned char)(digit - (unsigned char)'0');
        if (parsed > (UINT16_MAX - (uint32_t)digit) / 10U) {
            return false;
        }
        parsed = parsed * 10U + (uint32_t)digit;
    }
    if (parsed == 0U) {
        return false;
    }
    *port = (uint16_t)parsed;
    return true;
}

static bool connect_loopback(test_client *client) {
    struct sockaddr_in address;
    struct timeval timeout = {.tv_sec = 3, .tv_usec = 0};

    memset(client, 0, sizeof(*client));
    client->fd = socket(AF_INET, SOCK_STREAM, 0);
    if (client->fd < 0) {
        return false;
    }
    if (setsockopt(client->fd, SOL_SOCKET, SO_RCVTIMEO, &timeout,
                   sizeof(timeout)) < 0 ||
        setsockopt(client->fd, SOL_SOCKET, SO_SNDTIMEO, &timeout,
                   sizeof(timeout)) < 0) {
        close(client->fd);
        client->fd = -1;
        return false;
    }

    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(test_port);
    if (connect(client->fd, (const struct sockaddr *)&address,
                sizeof(address)) < 0) {
        close(client->fd);
        client->fd = -1;
        return false;
    }
    return true;
}

static void close_client(test_client *client) {
    if (client->fd >= 0) {
        close(client->fd);
        client->fd = -1;
    }
}

static bool send_all(int fd, const char *data, size_t length) {
    size_t sent_total = 0;
    while (sent_total < length) {
        ssize_t sent = send(fd, data + sent_total, length - sent_total, 0);
        if (sent > 0) {
            sent_total += (size_t)sent;
        } else if (sent < 0 && errno == EINTR) {
            continue;
        } else {
            return false;
        }
    }
    return true;
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

static unsigned char ascii_lower(unsigned char value) {
    if (value >= (unsigned char)'A' && value <= (unsigned char)'Z') {
        return (unsigned char)(value + ((unsigned char)'a' - (unsigned char)'A'));
    }
    return value;
}

static bool header_name_is(const char *data, size_t length, const char *name) {
    size_t name_len = strlen(name);
    if (length != name_len) {
        return false;
    }
    for (size_t i = 0; i < length; i++) {
        if (ascii_lower((unsigned char)data[i]) !=
            ascii_lower((unsigned char)name[i])) {
            return false;
        }
    }
    return true;
}

static bool parse_response_status(const char *data,
                                 size_t line_end,
                                 int *status) {
    if (line_end < 12U || memcmp(data, "HTTP/1.1 ", 9U) != 0 ||
        data[9] < '0' || data[9] > '9' || data[10] < '0' || data[10] > '9' ||
        data[11] < '0' || data[11] > '9') {
        return false;
    }
    *status = (data[9] - '0') * 100 + (data[10] - '0') * 10 +
              (data[11] - '0');
    return true;
}

static bool parse_content_length(const char *data,
                                 size_t separator,
                                 size_t *content_length) {
    size_t status_line_end = find_crlf(data, 0, separator + 2U);
    size_t position;
    bool found = false;

    if (status_line_end == SIZE_MAX) {
        return false;
    }
    position = status_line_end + 2U;
    while (position < separator) {
        size_t line_end = find_crlf(data, position, separator + 2U);
        size_t colon = position;
        const char *value;
        size_t value_len;
        size_t parsed = 0;

        if (line_end == SIZE_MAX || line_end == position) {
            return false;
        }
        while (colon < line_end && data[colon] != ':') {
            colon++;
        }
        if (colon == position || colon == line_end) {
            return false;
        }
        if (header_name_is(data + position, colon - position,
                           "content-length")) {
            if (found) {
                return false;
            }
            value = data + colon + 1U;
            value_len = line_end - colon - 1U;
            while (value_len > 0U &&
                   (*value == ' ' || *value == '\t')) {
                value++;
                value_len--;
            }
            while (value_len > 0U &&
                   (value[value_len - 1U] == ' ' ||
                    value[value_len - 1U] == '\t')) {
                value_len--;
            }
            if (value_len == 0U) {
                return false;
            }
            for (size_t i = 0; i < value_len; i++) {
                unsigned char digit = (unsigned char)value[i];
                if (digit < (unsigned char)'0' || digit > (unsigned char)'9') {
                    return false;
                }
                digit = (unsigned char)(digit - (unsigned char)'0');
                if (parsed > (SIZE_MAX - (size_t)digit) / 10U) {
                    return false;
                }
                parsed = parsed * 10U + (size_t)digit;
            }
            *content_length = parsed;
            found = true;
        }
        position = line_end + 2U;
    }
    return found;
}

static bool receive_response_details(test_client *client,
                                     int *status,
                                     size_t *header_length_out,
                                     size_t *body_length_out) {
    size_t separator = find_header_separator(client->buffer, client->length);
    size_t content_length = 0;
    size_t header_len;
    size_t expected_len;
    size_t status_line_end;

    while (separator == SIZE_MAX) {
        ssize_t received;
        if (client->length == sizeof(client->buffer)) {
            return false;
        }
        received = recv(client->fd, client->buffer + client->length,
                        sizeof(client->buffer) - client->length, 0);
        if (received > 0) {
            client->length += (size_t)received;
            separator = find_header_separator(client->buffer, client->length);
        } else if (received < 0 && errno == EINTR) {
            continue;
        } else {
            return false;
        }
    }

    status_line_end = find_crlf(client->buffer, 0, separator + 2U);
    if (status_line_end == SIZE_MAX ||
        !parse_response_status(client->buffer, status_line_end, status) ||
        !parse_content_length(client->buffer, separator, &content_length)) {
        return false;
    }
    header_len = separator + 4U;
    if (content_length > sizeof(client->buffer) - header_len) {
        return false;
    }
    expected_len = header_len + content_length;
    if (header_length_out != NULL) {
        *header_length_out = header_len;
    }
    if (body_length_out != NULL) {
        *body_length_out = content_length;
    }
    if (client->length > expected_len) {
        memmove(client->buffer, client->buffer + expected_len,
                client->length - expected_len);
        client->length -= expected_len;
    } else {
        while (client->length < expected_len) {
            ssize_t received = recv(client->fd,
                                    client->buffer + client->length,
                                    expected_len - client->length, 0);
            if (received > 0) {
                client->length += (size_t)received;
            } else if (received < 0 && errno == EINTR) {
                continue;
            } else {
                return false;
            }
        }
        client->length = 0;
    }
    return true;
}

static bool receive_response(test_client *client, int *status) {
    return receive_response_details(client, status, NULL, NULL);
}

static bool expect_status(const char *name,
                          const char *request,
                          size_t request_len,
                          int expected_status) {
    test_client client;
    int status = 0;
    bool passed = connect_loopback(&client);

    if (passed) {
        passed = send_all(client.fd, request, request_len) &&
                 receive_response(&client, &status) &&
                 status == expected_status;
    }
    close_client(&client);
    checks_run++;
    if (!passed) {
        checks_failed++;
        fprintf(stderr, "FAIL %-32s expected HTTP %d, received %d\n", name,
                expected_status, status);
    } else {
        printf("PASS %s (HTTP %d)\n", name, status);
    }
    return passed;
}

static bool contains_bytes(const char *data,
                          size_t data_len,
                          const char *fragment) {
    size_t fragment_len = strlen(fragment);
    if (fragment_len > data_len) {
        return false;
    }
    for (size_t i = 0U; i <= data_len - fragment_len; i++) {
        if (memcmp(data + i, fragment, fragment_len) == 0) {
            return true;
        }
    }
    return false;
}

static bool expect_response_content(const char *name,
                                    const char *request,
                                    size_t request_len,
                                    int expected_status,
                                    const char *content_type,
                                    const char *const *body_fragments,
                                    size_t body_fragment_count) {
    test_client client;
    int status = 0;
    size_t header_len = 0U;
    size_t body_len = 0U;
    bool passed = connect_loopback(&client);

    if (passed) {
        passed = send_all(client.fd, request, request_len) &&
                 receive_response_details(&client, &status, &header_len,
                                          &body_len) &&
                 status == expected_status;
    }
    if (passed) {
        passed = contains_bytes(client.buffer, header_len, content_type);
        for (size_t i = 0U; passed && i < body_fragment_count; i++) {
            passed = contains_bytes(client.buffer + header_len, body_len,
                                    body_fragments[i]);
        }
    }
    close_client(&client);
    checks_run++;
    if (!passed) {
        checks_failed++;
        fprintf(stderr, "FAIL %-32s expected HTTP %d, received %d\n", name,
                expected_status, status);
    } else {
        printf("PASS %s (HTTP %d, content verified)\n", name, status);
    }
    return passed;
}

static void test_favicon_route(void) {
    static const char request[] =
        "GET /favicon.ico HTTP/1.1\r\nHost: localhost\r\n"
        "Connection: close\r\n\r\n";
    static const unsigned char ico_header[] = {0U, 0U, 1U, 0U, 1U, 0U};
    test_client client;
    int status = 0;
    size_t header_len = 0U;
    size_t body_len = 0U;
    bool passed = connect_loopback(&client);

    if (passed) {
        passed = send_all(client.fd, request, sizeof(request) - 1U) &&
                 receive_response_details(&client, &status, &header_len,
                                          &body_len) &&
                 status == 200;
    }
    if (passed) {
        passed = contains_bytes(client.buffer, header_len,
                                "Content-Type: image/x-icon") &&
                 body_len >= sizeof(ico_header) &&
                 memcmp(client.buffer + header_len, ico_header,
                        sizeof(ico_header)) == 0;
    }
    close_client(&client);
    checks_run++;
    if (!passed) {
        checks_failed++;
        fprintf(stderr, "FAIL favicon route expected HTTP 200 and ICO data, "
                        "received %d\n",
                status);
    } else {
        puts("PASS favicon route (ICO response verified)");
    }
}

static void test_documentation_routes(void) {
    static const char docs_request[] =
        "GET /docs HTTP/1.1\r\nHost: localhost\r\n"
        "Connection: close\r\n\r\n";
    static const char docs_slash_request[] =
        "GET /docs/ HTTP/1.1\r\nHost: localhost\r\n"
        "Connection: close\r\n\r\n";
    static const char openapi_request[] =
        "GET /openapi.json HTTP/1.1\r\nHost: localhost\r\n"
        "Connection: close\r\n\r\n";
    static const char docs_method_not_allowed[] =
        "POST /docs HTTP/1.1\r\nHost: localhost\r\n"
        "Content-Length: 0\r\nConnection: close\r\n\r\n";
    static const char openapi_method_not_allowed[] =
        "POST /openapi.json HTTP/1.1\r\nHost: localhost\r\n"
        "Content-Length: 0\r\nConnection: close\r\n\r\n";
    static const char *const docs_fragments[] = {
        "swagger-ui-bundle.js", "swagger-ui-standalone-preset.js",
        "/openapi.json", "/favicon.ico"};
    static const char *const openapi_fragments[] = {
        "\"openapi\": \"3.0.3\"", "\"/keys/{key}\"",
        "X-TTL-Seconds", "\"/stats\""};

    (void)expect_response_content("Swagger UI docs", docs_request,
                                  sizeof(docs_request) - 1U, 200,
                                  "Content-Type: text/html; charset=utf-8",
                                  docs_fragments,
                                  sizeof(docs_fragments) / sizeof(docs_fragments[0]));
    (void)expect_response_content("Swagger UI trailing slash",
                                  docs_slash_request,
                                  sizeof(docs_slash_request) - 1U, 200,
                                  "Content-Type: text/html; charset=utf-8",
                                  docs_fragments,
                                  sizeof(docs_fragments) / sizeof(docs_fragments[0]));
    (void)expect_response_content("OpenAPI specification", openapi_request,
                                  sizeof(openapi_request) - 1U, 200,
                                  "Content-Type: application/json; charset=utf-8",
                                  openapi_fragments,
                                  sizeof(openapi_fragments) /
                                      sizeof(openapi_fragments[0]));
    (void)expect_status("docs method not allowed", docs_method_not_allowed,
                        sizeof(docs_method_not_allowed) - 1U, 405);
    (void)expect_status("OpenAPI method not allowed",
                        openapi_method_not_allowed,
                        sizeof(openapi_method_not_allowed) - 1U, 405);
    test_favicon_route();
}

static bool test_keep_alive(void) {
    test_client client;
    bool passed = connect_loopback(&client);
    int status = 0;

    if (passed) {
        for (size_t i = 0; i < 10U; i++) {
            if (!send_all(client.fd, stats_request, sizeof(stats_request) - 1U) ||
                !receive_response(&client, &status) || status != 200) {
                passed = false;
                break;
            }
        }
    }
    close_client(&client);
    checks_run++;
    if (!passed) {
        checks_failed++;
        fprintf(stderr, "FAIL keep-alive sequence (HTTP %d)\n", status);
    } else {
        puts("PASS keep-alive sequence (10 requests)");
    }
    return passed;
}

static bool test_pipelining(void) {
    static const char pipelined_requests[] =
        "GET /stats HTTP/1.1\r\nHost: localhost\r\n\r\n"
        "GET /stats HTTP/1.1\r\nHost: localhost\r\n\r\n";
    test_client client;
    int first_status = 0;
    int second_status = 0;
    bool passed = connect_loopback(&client);

    if (passed) {
        passed = send_all(client.fd, pipelined_requests,
                          sizeof(pipelined_requests) - 1U) &&
                 receive_response(&client, &first_status) &&
                 receive_response(&client, &second_status) &&
                 first_status == 200 && second_status == 200;
    }
    close_client(&client);
    checks_run++;
    if (!passed) {
        checks_failed++;
        fprintf(stderr, "FAIL HTTP pipelining (%d, %d)\n", first_status,
                second_status);
    } else {
        puts("PASS HTTP pipelining (2 responses)");
    }
    return passed;
}

static bool test_connection_close(void) {
    static const char request[] =
        "GET /stats HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Connection: close\r\n\r\n";
    test_client client;
    int status = 0;
    char byte;
    bool passed = connect_loopback(&client);

    if (passed) {
        passed = send_all(client.fd, request, sizeof(request) - 1U) &&
                 receive_response(&client, &status) && status == 200;
    }
    if (passed) {
        ssize_t received;
        do {
            received = recv(client.fd, &byte, sizeof(byte), 0);
        } while (received < 0 && errno == EINTR);
        passed = received == 0;
    }
    close_client(&client);
    checks_run++;
    if (!passed) {
        checks_failed++;
        fprintf(stderr, "FAIL Connection: close (HTTP %d)\n", status);
    } else {
        puts("PASS Connection: close");
    }
    return passed;
}

static bool test_large_header(void) {
    const char prefix[] =
        "GET /stats HTTP/1.1\r\nHost: localhost\r\nX-Pad: ";
    const char suffix[] = "\r\n\r\n";
    const size_t padding_len = 17U * 1024U;
    size_t request_len = sizeof(prefix) - 1U + padding_len + sizeof(suffix) - 1U;
    char *request = malloc(request_len);
    bool passed = false;

    if (request != NULL) {
        size_t offset = sizeof(prefix) - 1U;
        memcpy(request, prefix, offset);
        memset(request + offset, 'A', padding_len);
        offset += padding_len;
        memcpy(request + offset, suffix, sizeof(suffix) - 1U);
        passed = expect_status("header over configured limit", request,
                               request_len, 431);
        free(request);
        return passed;
    }
    checks_run++;
    checks_failed++;
    fprintf(stderr, "FAIL unable to allocate oversized-header case\n");
    return false;
}

static bool test_long_key(void) {
    const char prefix[] = "GET /keys/";
    const char suffix[] = " HTTP/1.1\r\nHost: localhost\r\n\r\n";
    const size_t key_len = 1025U;
    size_t request_len = sizeof(prefix) - 1U + key_len + sizeof(suffix) - 1U;
    char *request = malloc(request_len);
    bool passed = false;

    if (request != NULL) {
        size_t offset = sizeof(prefix) - 1U;
        memcpy(request, prefix, offset);
        memset(request + offset, 'k', key_len);
        offset += key_len;
        memcpy(request + offset, suffix, sizeof(suffix) - 1U);
        passed = expect_status("key over configured limit", request,
                               request_len, 400);
        free(request);
        return passed;
    }
    checks_run++;
    checks_failed++;
    fprintf(stderr, "FAIL unable to allocate long-key case\n");
    return false;
}

static bool test_control_byte(unsigned char value) {
    const char prefix[] =
        "GET /stats HTTP/1.1\r\nHost: localhost\r\nX-Input: ";
    const char suffix[] = "\r\n\r\n";
    char request[sizeof(prefix) + sizeof(suffix) - 1U];
    size_t offset = sizeof(prefix) - 1U;

    memcpy(request, prefix, offset);
    request[offset++] = (char)value;
    memcpy(request + offset, suffix, sizeof(suffix) - 1U);
    return expect_status("control byte in header", request, sizeof(request), 400);
}

typedef struct {
    bool failed;
} stress_worker;

static void *run_stress_worker(void *context) {
    stress_worker *worker = context;
    test_client client;
    int status = 0;

    if (!connect_loopback(&client)) {
        worker->failed = true;
        return NULL;
    }
    for (size_t i = 0; i < STRESS_REQUESTS_PER_THREAD; i++) {
        if (!send_all(client.fd, stats_request, sizeof(stats_request) - 1U) ||
            !receive_response(&client, &status) || status != 200) {
            worker->failed = true;
            break;
        }
    }
    close_client(&client);
    return NULL;
}

static bool test_concurrent_requests(void) {
    pthread_t threads[STRESS_THREAD_COUNT];
    stress_worker workers[STRESS_THREAD_COUNT] = {{false}};
    size_t started = 0;
    bool passed = true;

    for (; started < STRESS_THREAD_COUNT; started++) {
        int error = pthread_create(&threads[started], NULL, run_stress_worker,
                                   &workers[started]);
        if (error != 0) {
            fprintf(stderr, "pthread_create: %s\n", strerror(error));
            passed = false;
            break;
        }
    }
    for (size_t i = 0; i < started; i++) {
        int error = pthread_join(threads[i], NULL);
        if (error != 0) {
            fprintf(stderr, "pthread_join: %s\n", strerror(error));
            exit(EXIT_FAILURE);
        }
        if (workers[i].failed) {
            passed = false;
        }
    }
    checks_run++;
    if (!passed) {
        checks_failed++;
        fprintf(stderr, "FAIL concurrent local requests\n");
    } else {
        printf("PASS concurrent local requests (%u workers, %u requests each)\n",
               STRESS_THREAD_COUNT, STRESS_REQUESTS_PER_THREAD);
    }
    return passed;
}

int main(int argc, char **argv) {
    static const char malformed_line[] = "BAD\r\n\r\n";
    static const char unsupported_version[] =
        "GET /stats HTTP/1.0\r\nHost: localhost\r\n\r\n";
    static const char missing_host[] = "GET /stats HTTP/1.1\r\n\r\n";
    static const char duplicate_content_length[] =
        "GET /stats HTTP/1.1\r\nHost: localhost\r\n"
        "Content-Length: 0\r\nContent-Length: 0\r\n\r\n";
    static const char content_length_overflow[] =
        "GET /stats HTTP/1.1\r\nHost: localhost\r\n"
        "Content-Length: 18446744073709551616\r\n\r\n";
    static const char transfer_encoding[] =
        "GET /stats HTTP/1.1\r\nHost: localhost\r\n"
        "Transfer-Encoding: chunked\r\n\r\n";
    static const char expectation[] =
        "PUT /keys/no-write HTTP/1.1\r\nHost: localhost\r\n"
        "Content-Length: 0\r\nExpect: 100-continue\r\n\r\n";
    static const char duplicate_host[] =
        "GET /stats HTTP/1.1\r\nHost: localhost\r\n"
        "Host: localhost\r\n\r\n";
    static const char bad_header[] =
        "GET /stats HTTP/1.1\r\nHost: localhost\r\nBroken-Header\r\n\r\n";
    static const char method_not_allowed[] =
        "POST /keys HTTP/1.1\r\nHost: localhost\r\nContent-Length: 0\r\n\r\n";
    static const char query_string[] =
        "GET /stats?detail=1 HTTP/1.1\r\nHost: localhost\r\n\r\n";
    static const char invalid_escape[] =
        "GET /keys/%GG HTTP/1.1\r\nHost: localhost\r\n\r\n";
    static const char invalid_utf8_key[] =
        "GET /keys/%FF HTTP/1.1\r\nHost: localhost\r\n\r\n";
    static const char empty_key[] =
        "GET /keys/ HTTP/1.1\r\nHost: localhost\r\n\r\n";
    static const char wrong_content_type[] =
        "PUT /keys/no-write HTTP/1.1\r\nHost: localhost\r\n"
        "Content-Type: text/plain\r\nContent-Length: 1\r\n\r\nx";
    static const char invalid_json[] =
        "PUT /keys/no-write HTTP/1.1\r\nHost: localhost\r\n"
        "Content-Type: application/json\r\nContent-Length: 1\r\n\r\n{";
    static const char body_too_large[] =
        "PUT /keys/no-write HTTP/1.1\r\nHost: localhost\r\n"
        "Content-Type: application/json\r\nContent-Length: 1048577\r\n\r\n";
    static const char zero_ttl[] =
        "PUT /keys/no-write HTTP/1.1\r\nHost: localhost\r\n"
        "Content-Type: application/json\r\nContent-Length: 1\r\n"
        "X-TTL-Seconds: 0\r\n\r\n1";
    static const char negative_ttl[] =
        "PUT /keys/no-write HTTP/1.1\r\nHost: localhost\r\n"
        "Content-Type: application/json\r\nContent-Length: 1\r\n"
        "X-TTL-Seconds: -1\r\n\r\n1";
    static const char overflowing_ttl[] =
        "PUT /keys/no-write HTTP/1.1\r\nHost: localhost\r\n"
        "Content-Type: application/json\r\nContent-Length: 1\r\n"
        "X-TTL-Seconds: 18446744073709551616\r\n\r\n1";
    static const char duplicate_ttl[] =
        "PUT /keys/no-write HTTP/1.1\r\nHost: localhost\r\n"
        "Content-Type: application/json\r\nContent-Length: 1\r\n"
        "X-TTL-Seconds: 1\r\nX-TTL-Seconds: 2\r\n\r\n1";
    static const char ttl_on_get[] =
        "GET /stats HTTP/1.1\r\nHost: localhost\r\n"
        "X-TTL-Seconds: 1\r\n\r\n";
    static const char valid_stats[] =
        "GET /stats HTTP/1.1\r\nHost: localhost\r\n\r\n";
    if (argc > 2 || (argc == 2 && !parse_test_port(argv[1], &test_port))) {
        fprintf(stderr, "usage: %s [loopback-port]\n", argv[0]);
        return EXIT_FAILURE;
    }
    (void)signal(SIGPIPE, SIG_IGN);

    expect_status("valid stats request", valid_stats,
                  sizeof(valid_stats) - 1U, 200);
    test_documentation_routes();
    expect_status("malformed request line", malformed_line,
                  sizeof(malformed_line) - 1U, 400);
    expect_status("unsupported HTTP version", unsupported_version,
                  sizeof(unsupported_version) - 1U, 505);
    expect_status("missing Host header", missing_host,
                  sizeof(missing_host) - 1U, 400);
    expect_status("duplicate Host header", duplicate_host,
                  sizeof(duplicate_host) - 1U, 400);
    expect_status("duplicate Content-Length", duplicate_content_length,
                  sizeof(duplicate_content_length) - 1U, 400);
    expect_status("Content-Length overflow", content_length_overflow,
                  sizeof(content_length_overflow) - 1U, 400);
    expect_status("Transfer-Encoding rejected", transfer_encoding,
                  sizeof(transfer_encoding) - 1U, 400);
    expect_status("Expect rejected", expectation, sizeof(expectation) - 1U,
                  417);
    expect_status("malformed header field", bad_header,
                  sizeof(bad_header) - 1U, 400);
    expect_status("method not allowed", method_not_allowed,
                  sizeof(method_not_allowed) - 1U, 405);
    expect_status("query string rejected", query_string,
                  sizeof(query_string) - 1U, 400);
    expect_status("invalid percent escape", invalid_escape,
                  sizeof(invalid_escape) - 1U, 400);
    expect_status("invalid UTF-8 key", invalid_utf8_key,
                  sizeof(invalid_utf8_key) - 1U, 400);
    expect_status("empty key rejected", empty_key, sizeof(empty_key) - 1U,
                  400);
    expect_status("wrong content type", wrong_content_type,
                  sizeof(wrong_content_type) - 1U, 415);
    expect_status("invalid JSON body", invalid_json,
                  sizeof(invalid_json) - 1U, 400);
    expect_status("request body over limit", body_too_large,
                  sizeof(body_too_large) - 1U, 413);
    expect_status("zero TTL rejected", zero_ttl, sizeof(zero_ttl) - 1U, 400);
    expect_status("negative TTL rejected", negative_ttl,
                  sizeof(negative_ttl) - 1U, 400);
    expect_status("overflowing TTL rejected", overflowing_ttl,
                  sizeof(overflowing_ttl) - 1U, 400);
    expect_status("duplicate TTL header rejected", duplicate_ttl,
                  sizeof(duplicate_ttl) - 1U, 400);
    expect_status("TTL header only allowed for PUT", ttl_on_get,
                  sizeof(ttl_on_get) - 1U, 400);
    test_long_key();
    test_large_header();

    for (unsigned int value = 1U; value <= 0x1fU; value++) {
        if (value != (unsigned int)'\t') {
            test_control_byte((unsigned char)value);
        }
    }
    test_control_byte(0x7fU);
    test_keep_alive();
    test_pipelining();
    test_connection_close();
    test_concurrent_requests();
    expect_status("server remains responsive", valid_stats,
                  sizeof(valid_stats) - 1U, 200);

    printf("%zu robustness checks, %zu failed\n", checks_run, checks_failed);
    return checks_failed == 0U ? 0 : 1;
}
