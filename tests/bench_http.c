#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <inttypes.h>
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
#include <time.h>
#include <unistd.h>

#define DEFAULT_REQUESTS 100000U
#define DEFAULT_THREADS 4U
#define MAX_THREADS 32U
#define MAX_REQUESTS 10000000U
#define RESPONSE_BUFFER_SIZE 4096U

static const char request_bytes[] =
    "GET /stats HTTP/1.1\r\n"
    "Host: localhost\r\n"
    "Connection: keep-alive\r\n\r\n";

typedef struct {
    uint16_t port;
    size_t requests;
    uint64_t completed;
    uint64_t failed;
    uint64_t total_latency_ns;
    uint64_t max_latency_ns;
} worker_data;

static bool parse_size(const char *text, size_t *value) {
    char *end = NULL;
    unsigned long long parsed;

    if (text == NULL || *text == '\0' || *text == '-' || *text == '+') {
        return false;
    }
    errno = 0;
    parsed = strtoull(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || parsed == 0U ||
        parsed > SIZE_MAX) {
        return false;
    }
    *value = (size_t)parsed;
    return true;
}

static bool monotonic_nanoseconds(uint64_t *value) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        return false;
    }
    *value = (uint64_t)now.tv_sec * UINT64_C(1000000000) +
             (uint64_t)now.tv_nsec;
    return true;
}

static int connect_to_server(uint16_t port) {
    struct sockaddr_in address;
    struct timeval timeout = {.tv_sec = 5, .tv_usec = 0};
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }

    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(port);
    if (connect(fd, (const struct sockaddr *)&address, sizeof(address)) < 0 ||
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) < 0 ||
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) < 0) {
        close(fd);
        return -1;
    }
    return fd;
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

static size_t find_header_end(const char *data, size_t length) {
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

static bool parse_content_length(const char *data,
                                size_t separator,
                                size_t *content_length) {
    size_t first_line_end = find_crlf(data, 0, separator + 2U);
    size_t position;
    bool found = false;

    if (first_line_end == SIZE_MAX) {
        return false;
    }
    position = first_line_end + 2U;
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

static bool parse_status_code(const char *data,
                              size_t status_line_end,
                              int *status_code) {
    if (status_line_end < 12U || memcmp(data, "HTTP/1.1 ", 9U) != 0 ||
        data[9] < '0' || data[9] > '9' || data[10] < '0' || data[10] > '9' ||
        data[11] < '0' || data[11] > '9') {
        return false;
    }
    *status_code = (data[9] - '0') * 100 + (data[10] - '0') * 10 +
                   (data[11] - '0');
    return true;
}

static bool receive_response(int fd, int *status_code) {
    char response[RESPONSE_BUFFER_SIZE];
    size_t received = 0;
    size_t separator = SIZE_MAX;
    size_t content_length = 0;
    size_t header_len;
    size_t expected_len;
    size_t status_line_end;

    while (separator == SIZE_MAX) {
        ssize_t count;
        if (received == sizeof(response)) {
            return false;
        }
        count = recv(fd, response + received, sizeof(response) - received, 0);
        if (count > 0) {
            received += (size_t)count;
            separator = find_header_end(response, received);
        } else if (count < 0 && errno == EINTR) {
            continue;
        } else {
            return false;
        }
    }

    status_line_end = find_crlf(response, 0, separator + 2U);
    if (status_line_end == SIZE_MAX ||
        !parse_status_code(response, status_line_end, status_code) ||
        !parse_content_length(response, separator, &content_length)) {
        return false;
    }
    header_len = separator + 4U;
    if (content_length > sizeof(response) - header_len) {
        return false;
    }
    expected_len = header_len + content_length;
    if (received > expected_len) {
        return false;
    }
    while (received < expected_len) {
        ssize_t count = recv(fd, response + received, expected_len - received, 0);
        if (count > 0) {
            received += (size_t)count;
        } else if (count < 0 && errno == EINTR) {
            continue;
        } else {
            return false;
        }
    }
    return true;
}

static void *run_worker(void *context) {
    worker_data *worker = context;
    int fd = connect_to_server(worker->port);

    if (fd < 0) {
        worker->failed = (uint64_t)worker->requests;
        return NULL;
    }

    for (size_t i = 0; i < worker->requests; i++) {
        uint64_t start;
        uint64_t end;
        int status_code = 0;
        bool succeeded;

        if (!monotonic_nanoseconds(&start)) {
            worker->failed += (uint64_t)(worker->requests - i);
            break;
        }
        succeeded = send_all(fd, request_bytes, sizeof(request_bytes) - 1U) &&
                    receive_response(fd, &status_code) && status_code == 200;
        if (!monotonic_nanoseconds(&end)) {
            worker->failed += (uint64_t)(worker->requests - i);
            break;
        }
        if (!succeeded) {
            worker->failed += (uint64_t)(worker->requests - i);
            break;
        }
        uint64_t latency = end - start;
        worker->completed++;
        worker->total_latency_ns += latency;
        if (latency > worker->max_latency_ns) {
            worker->max_latency_ns = latency;
        }
    }

    close(fd);
    return NULL;
}

int main(int argc, char **argv) {
    uint16_t port = 8080U;
    size_t requests = DEFAULT_REQUESTS;
    size_t thread_count = DEFAULT_THREADS;
    pthread_t *threads = NULL;
    worker_data *workers = NULL;
    uint64_t start;
    uint64_t end;
    uint64_t completed = 0;
    uint64_t failed = 0;
    uint64_t total_latency_ns = 0;
    uint64_t max_latency_ns = 0;
    size_t created_threads = 0;
    int result = 1;

    if (argc > 4) {
        fprintf(stderr,
                "Usage: %s [port] [requests=1..%u] [threads=1..%u]\n",
                argv[0], MAX_REQUESTS, MAX_THREADS);
        return 2;
    }
    if (argc >= 2) {
        size_t parsed_port;
        if (!parse_size(argv[1], &parsed_port) || parsed_port > 65535U) {
            fprintf(stderr, "Port must be in the range 1..65535\n");
            return 2;
        }
        port = (uint16_t)parsed_port;
    }
    if ((argc >= 3 && !parse_size(argv[2], &requests)) ||
        (argc >= 4 && !parse_size(argv[3], &thread_count)) ||
        requests > MAX_REQUESTS || thread_count > MAX_THREADS ||
        thread_count > requests) {
        fprintf(stderr,
                "Usage: %s [port] [requests=1..%u] [threads=1..%u]\n",
                argv[0], MAX_REQUESTS, MAX_THREADS);
        return 2;
    }

    threads = calloc(thread_count, sizeof(*threads));
    workers = calloc(thread_count, sizeof(*workers));
    if (threads == NULL || workers == NULL) {
        fprintf(stderr, "Unable to allocate benchmark workers\n");
        goto cleanup;
    }
    (void)signal(SIGPIPE, SIG_IGN);

    size_t base_requests = requests / thread_count;
    size_t extra_requests = requests % thread_count;
    for (size_t i = 0; i < thread_count; i++) {
        workers[i].port = port;
        workers[i].requests = base_requests + (i < extra_requests ? 1U : 0U);
    }
    if (!monotonic_nanoseconds(&start)) {
        perror("clock_gettime");
        goto cleanup;
    }
    for (; created_threads < thread_count; created_threads++) {
        int error = pthread_create(&threads[created_threads], NULL, run_worker,
                                   &workers[created_threads]);
        if (error != 0) {
            fprintf(stderr, "pthread_create: %s\n", strerror(error));
            for (size_t i = created_threads; i < thread_count; i++) {
                workers[i].failed = (uint64_t)workers[i].requests;
            }
            break;
        }
    }
    for (size_t i = 0; i < created_threads; i++) {
        int error = pthread_join(threads[i], NULL);
        if (error != 0) {
            fprintf(stderr, "pthread_join: %s\n", strerror(error));
            exit(EXIT_FAILURE);
        }
    }
    if (!monotonic_nanoseconds(&end)) {
        perror("clock_gettime");
        goto cleanup;
    }

    for (size_t i = 0; i < thread_count; i++) {
        completed += workers[i].completed;
        failed += workers[i].failed;
        total_latency_ns += workers[i].total_latency_ns;
        if (workers[i].max_latency_ns > max_latency_ns) {
            max_latency_ns = workers[i].max_latency_ns;
        }
    }

    double elapsed_seconds = (double)(end - start) / 1000000000.0;
    double requests_per_second = elapsed_seconds > 0.0
                                    ? (double)completed / elapsed_seconds
                                    : 0.0;
    double average_latency_ms = completed > 0U
                                    ? (double)total_latency_ns /
                                          (double)completed / 1000000.0
                                    : 0.0;
    double maximum_latency_ms = (double)max_latency_ns / 1000000.0;

    printf("HTTP benchmark: GET /stats target=127.0.0.1:%u requested=%zu workers=%zu\n",
           (unsigned int)port, requests, thread_count);
    printf("completed=%" PRIu64 " failed=%" PRIu64
           " elapsed=%.3f s throughput=%.0f req/s\n",
           completed, failed, elapsed_seconds, requests_per_second);
    printf("latency average=%.3f ms max=%.3f ms\n", average_latency_ms,
           maximum_latency_ms);
    result = failed == 0U && completed == (uint64_t)requests ? 0 : 1;

cleanup:
    free(workers);
    free(threads);
    return result;
}
