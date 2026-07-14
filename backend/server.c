/*
 * Motion CAPTCHA backend server.
 *
 * Plain C, POSIX sockets, no external web framework.
 * Serves the frontend static files and exposes two JSON endpoints:
 *   GET  /api/challenge  -> issues a new challenge + session token
 *   POST /api/verify     -> verifies submitted motion sensor data
 *
 * Every issued challenge and every verification attempt is logged to
 * logs/verification_log.csv for later analysis (accuracy, timing, etc).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <strings.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/stat.h>

#include "cJSON.h"
#include "challenge.h"
#include "verify.h"

#define PORT 8080
#define BACKLOG 32
#define MAX_REQUEST 1048576      /* 1 MB max request size */
#define SESSION_TTL_SECONDS 120
#define MAX_SESSIONS 4096
#define STATIC_DIR "../frontend"
#define LOG_PATH "../logs/verification_log.csv"
#define SAMPLES_DIR "../logs/samples"

/* ---------------- session store ---------------- */

typedef struct {
    char token[37];
    challenge_type_t type;
    time_t issued_at;
    int used;
    int valid;
} session_t;

static session_t g_sessions[MAX_SESSIONS];
static pthread_mutex_t g_sessions_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_log_lock = PTHREAD_MUTEX_INITIALIZER;

static void gen_token(char *out /* 37 bytes */) {
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < 36; i++) {
        out[i] = hex[rand() % 16];
    }
    out[8] = out[13] = out[18] = out[23] = '-';
    out[36] = '\0';
}

static session_t *session_create(challenge_type_t type) {
    pthread_mutex_lock(&g_sessions_lock);
    time_t now = time(NULL);
    session_t *slot = NULL;
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (!g_sessions[i].valid || (now - g_sessions[i].issued_at) > SESSION_TTL_SECONDS) {
            slot = &g_sessions[i];
            break;
        }
    }
    if (!slot) slot = &g_sessions[0]; /* fallback: overwrite oldest slot */
    gen_token(slot->token);
    slot->type = type;
    slot->issued_at = now;
    slot->used = 0;
    slot->valid = 1;
    session_t copy = *slot;
    pthread_mutex_unlock(&g_sessions_lock);
    /* return pointer valid only for token/type copy purposes below */
    static __thread session_t tls_copy;
    tls_copy = copy;
    return &tls_copy;
}

/* Returns 1 and fills *type if token is valid & unused & not expired.
 * Marks it used (each token can only be verified once). */
static int session_consume(const char *token, challenge_type_t *type) {
    pthread_mutex_lock(&g_sessions_lock);
    time_t now = time(NULL);
    int ok = 0;
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (g_sessions[i].valid && strcmp(g_sessions[i].token, token) == 0) {
            if (!g_sessions[i].used && (now - g_sessions[i].issued_at) <= SESSION_TTL_SECONDS) {
                *type = g_sessions[i].type;
                g_sessions[i].used = 1;
                ok = 1;
            }
            break;
        }
    }
    pthread_mutex_unlock(&g_sessions_lock);
    return ok;
}

/* ---------------- logging ---------------- */

static void log_init(void) {
    mkdir("../logs", 0755);
    mkdir(SAMPLES_DIR, 0755);
    struct stat st;
    if (stat(LOG_PATH, &st) != 0) {
        FILE *f = fopen(LOG_PATH, "w");
        if (f) {
            fprintf(f, "timestamp,token,challenge,samples,duration_ms,success,confidence,response_time_ms,reason,samples_file\n");
            fclose(f);
        }
    }
}

/* Saves the full raw sensor sample set for one verification session to its
 * own JSON file, named after the verification token, so the main CSV log
 * can stay concise while all experimental data is preserved for debugging
 * and evaluation (spec section 11). Returns the relative path written, or
 * an empty string if nothing was written. */
static void save_raw_samples(const char *token, const char *challenge,
                              const char *body, char *out_path, size_t out_path_len) {
    out_path[0] = '\0';
    if (!token || !token[0]) return;

    char path[256];
    snprintf(path, sizeof(path), "%s/%s.json", SAMPLES_DIR, token);

    FILE *f = fopen(path, "w");
    if (!f) return;

    time_t now = time(NULL);
    char tbuf[32];
    strftime(tbuf, sizeof(tbuf), "%Y-%m-%dT%H:%M:%S", localtime(&now));

    fprintf(f, "{\n  \"token\": \"%s\",\n  \"challenge\": \"%s\",\n  \"timestamp\": \"%s\",\n  \"raw_request\": %s\n}\n",
            token, challenge ? challenge : "", tbuf, body && body[0] ? body : "null");
    fclose(f);

    snprintf(out_path, out_path_len, "samples/%s.json", token);
}

static void log_event(const char *token, const char *challenge, int n_samples,
                       double duration_ms, int success, double confidence,
                       double response_time_ms, const char *reason,
                       const char *samples_file) {
    pthread_mutex_lock(&g_log_lock);
    FILE *f = fopen(LOG_PATH, "a");
    if (f) {
        time_t now = time(NULL);
        char tbuf[32];
        strftime(tbuf, sizeof(tbuf), "%Y-%m-%dT%H:%M:%S", localtime(&now));
        /* escape commas in reason by replacing with ';' to keep valid CSV */
        char safe_reason[256];
        strncpy(safe_reason, reason, sizeof(safe_reason) - 1);
        safe_reason[sizeof(safe_reason) - 1] = '\0';
        for (char *p = safe_reason; *p; p++) if (*p == ',') *p = ';';
        fprintf(f, "%s,%s,%s,%d,%.1f,%d,%.1f,%.1f,%s,%s\n",
                tbuf, token, challenge, n_samples, duration_ms, success, confidence,
                response_time_ms, safe_reason, samples_file ? samples_file : "");
        fclose(f);
    }
    pthread_mutex_unlock(&g_log_lock);
}

/* ---------------- tiny HTTP helpers ---------------- */

static char *strcasestr_local(const char *haystack, const char *needle);

typedef struct {
    char method[8];
    char path[512];
    char *body;
    long content_length;
} http_request_t;

static void send_response(int fd, int status, const char *status_text,
                           const char *content_type, const char *body, long body_len) {
    char header[512];
    int hlen = snprintf(header, sizeof(header),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %ld\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
        "Access-Control-Allow-Headers: Content-Type\r\n"
        "Connection: close\r\n"
        "\r\n",
        status, status_text, content_type, body_len);
    write(fd, header, hlen);
    if (body_len > 0) write(fd, body, body_len);
}

static void send_json(int fd, int status, const char *status_text, json_value_t *v) {
    char *s = json_serialize(v);
    send_response(fd, status, status_text, "application/json", s, (long)strlen(s));
    free(s);
}

static const char *content_type_for(const char *path) {
    size_t len = strlen(path);
    if (len >= 5 && strcmp(path + len - 5, ".html") == 0) return "text/html";
    if (len >= 3 && strcmp(path + len - 3, ".js") == 0) return "application/javascript";
    if (len >= 4 && strcmp(path + len - 4, ".css") == 0) return "text/css";
    if (len >= 5 && strcmp(path + len - 5, ".json") == 0) return "application/json";
    return "application/octet-stream";
}

static void serve_static(int fd, const char *req_path) {
    char full[600];
    if (strcmp(req_path, "/") == 0) req_path = "/index.html";
    snprintf(full, sizeof(full), "%s%s", STATIC_DIR, req_path);

    /* prevent path traversal */
    if (strstr(req_path, "..")) {
        const char *msg = "Forbidden";
        send_response(fd, 403, "Forbidden", "text/plain", msg, (long)strlen(msg));
        return;
    }

    FILE *f = fopen(full, "rb");
    if (!f) {
        const char *msg = "Not Found";
        send_response(fd, 404, "Not Found", "text/plain", msg, (long)strlen(msg));
        return;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc(size);
    size_t rd = fread(buf, 1, size, f);
    (void)rd;
    fclose(f);
    send_response(fd, 200, "OK", content_type_for(full), buf, size);
    free(buf);
}

/* Reads the raw HTTP request (headers + body) from the socket into a buffer. */
static char *read_full_request(int fd, long *out_len) {
    char *buf = malloc(MAX_REQUEST);
    long total = 0;
    long content_length = -1;
    long header_end = -1;

    while (total < MAX_REQUEST - 1) {
        long n = read(fd, buf + total, MAX_REQUEST - 1 - total);
        if (n <= 0) break;
        total += n;
        buf[total] = '\0';

        if (header_end < 0) {
            char *p = strstr(buf, "\r\n\r\n");
            if (p) {
                header_end = (p - buf) + 4;
                char *cl = strcasestr_local(buf, "Content-Length:");
                if (cl) content_length = atol(cl + strlen("Content-Length:"));
                else content_length = 0;
            }
        }
        if (header_end >= 0 && (total - header_end) >= content_length) {
            break;
        }
    }
    *out_len = total;
    return buf;
}

/* strcasestr is a GNU extension; provide a portable fallback name to avoid
 * relying on nonstandard headers being declared. */
static char *strcasestr_local(const char *haystack, const char *needle) {
    size_t nlen = strlen(needle);
    for (const char *p = haystack; *p; p++) {
        if (strncasecmp(p, needle, nlen) == 0) return (char *)p;
    }
    return NULL;
}

static int parse_request(char *raw, long raw_len, http_request_t *req) {
    char *line_end = strstr(raw, "\r\n");
    if (!line_end) return -1;
    *line_end = '\0';

    char path[512];
    if (sscanf(raw, "%7s %511s", req->method, path) != 2) return -1;
    strncpy(req->path, path, sizeof(req->path) - 1);

    char *headers = line_end + 2;
    char *header_end = strstr(headers, "\r\n\r\n");
    req->content_length = 0;
    if (header_end) {
        char *cl = strcasestr_local(raw, "Content-Length:");
        if (cl) req->content_length = atol(cl + strlen("Content-Length:"));
        req->body = header_end + 4;
    } else {
        req->body = NULL;
    }
    (void)raw_len;
    return 0;
}

/* ---------------- API handlers ---------------- */

static void handle_challenge(int fd) {
    challenge_type_t type = challenge_pick_random();
    session_t s = *session_create(type);

    json_value_t *resp = json_new_object();
    json_object_set(resp, "token", json_new_string(s.token));
    json_object_set(resp, "challenge", json_new_string(challenge_type_name(type)));
    json_object_set(resp, "instruction", json_new_string(challenge_instruction(type)));
    json_object_set(resp, "ttl_seconds", json_new_number(SESSION_TTL_SECONDS));

    log_event(s.token, challenge_type_name(type), 0, 0, -1, 0, 0, "challenge_issued", "");

    send_json(fd, 200, "OK", resp);
    json_free(resp);
}

static void handle_verify(int fd, const char *body) {
    struct timespec t_start, t_end;
    clock_gettime(CLOCK_MONOTONIC, &t_start);

    json_value_t *root = json_parse(body);
    if (!root) {
        json_value_t *err = json_new_object();
        json_object_set(err, "error", json_new_string("invalid_json"));
        send_json(fd, 400, "Bad Request", err);
        json_free(err);
        return;
    }

    const char *token = json_get_string(json_object_get(root, "token"));
    json_value_t *samples_arr = json_object_get(root, "samples");

    if (!token || !samples_arr || samples_arr->type != JSON_ARRAY) {
        json_value_t *err = json_new_object();
        json_object_set(err, "error", json_new_string("missing_fields"));
        send_json(fd, 400, "Bad Request", err);
        json_free(err);
        json_free(root);
        return;
    }

    challenge_type_t type;
    if (!session_consume(token, &type)) {
        json_value_t *err = json_new_object();
        json_object_set(err, "success", json_new_bool(0));
        json_object_set(err, "error", json_new_string("invalid_or_expired_token"));
        send_json(fd, 200, "OK", err);
        json_free(err);
        json_free(root);
        return;
    }

    /* count samples */
    int n = 0;
    for (json_value_t *c = samples_arr->child; c; c = c->next) n++;

    motion_sample_t *samples = NULL;
    if (n > 0) {
        samples = calloc(n, sizeof(motion_sample_t));
        int i = 0;
        for (json_value_t *c = samples_arr->child; c; c = c->next, i++) {
            samples[i].t_ms  = json_get_number(json_object_get(c, "t"), 0);
            samples[i].alpha = json_get_number(json_object_get(c, "alpha"), 0);
            samples[i].beta  = json_get_number(json_object_get(c, "beta"), 0);
            samples[i].gamma = json_get_number(json_object_get(c, "gamma"), 0);
            samples[i].ax    = json_get_number(json_object_get(c, "ax"), 0);
            samples[i].ay    = json_get_number(json_object_get(c, "ay"), 0);
            samples[i].az    = json_get_number(json_object_get(c, "az"), 0);
        }
    }

    /* Calibration baseline captured before the challenge was shown (spec:
     * always compare movement to the calibrated starting position, not an
     * assumed upright start). Optional - falls back gracefully if absent. */
    calibration_t calibration = { 0.0, 0.0, 0 };
    json_value_t *cal_obj = json_object_get(root, "calibration");
    if (cal_obj && cal_obj->type == JSON_OBJECT) {
        calibration.beta = json_get_number(json_object_get(cal_obj, "beta"), 0);
        calibration.gamma = json_get_number(json_object_get(cal_obj, "gamma"), 0);
        calibration.valid = 1;
    }

    verify_result_t result;
    verify_run(type, samples, n, &calibration, &result);
    free(samples);

    clock_gettime(CLOCK_MONOTONIC, &t_end);
    double response_ms = (t_end.tv_sec - t_start.tv_sec) * 1000.0 +
                          (t_end.tv_nsec - t_start.tv_nsec) / 1e6;

    char samples_file[128];
    save_raw_samples(token, challenge_type_name(type), body, samples_file, sizeof(samples_file));

    log_event(token, challenge_type_name(type), n, result.duration_ms,
              result.success, result.confidence, response_ms, result.reason,
              samples_file);

    json_value_t *resp = json_new_object();
    json_object_set(resp, "success", json_new_bool(result.success));
    json_object_set(resp, "confidence", json_new_number(result.confidence));
    json_object_set(resp, "duration_ms", json_new_number(result.duration_ms));
    json_object_set(resp, "reason", json_new_string(result.reason));
    json_object_set(resp, "challenge", json_new_string(challenge_type_name(type)));

    send_json(fd, 200, "OK", resp);
    json_free(resp);
    json_free(root);
}

/* ---------------- connection handling ---------------- */

static void *handle_connection(void *arg) {
    int fd = *(int *)arg;
    free(arg);

    long raw_len = 0;
    char *raw = read_full_request(fd, &raw_len);
    if (raw_len == 0) { close(fd); free(raw); return NULL; }

    http_request_t req;
    memset(&req, 0, sizeof(req));
    if (parse_request(raw, raw_len, &req) != 0) {
        const char *msg = "Bad Request";
        send_response(fd, 400, "Bad Request", "text/plain", msg, (long)strlen(msg));
        close(fd); free(raw); return NULL;
    }

    if (strcmp(req.method, "OPTIONS") == 0) {
        send_response(fd, 204, "No Content", "text/plain", "", 0);
    } else if (strcmp(req.method, "GET") == 0 && strcmp(req.path, "/api/challenge") == 0) {
        handle_challenge(fd);
    } else if (strcmp(req.method, "POST") == 0 && strcmp(req.path, "/api/verify") == 0) {
        handle_verify(fd, req.body ? req.body : "");
    } else if (strcmp(req.method, "GET") == 0) {
        serve_static(fd, req.path);
    } else {
        const char *msg = "Method Not Allowed";
        send_response(fd, 405, "Method Not Allowed", "text/plain", msg, (long)strlen(msg));
    }

    close(fd);
    free(raw);
    return NULL;
}

int main(void) {
    srand((unsigned int)time(NULL));
    log_init();

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) { perror("socket"); return 1; }

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(PORT);

    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        return 1;
    }
    if (listen(server_fd, BACKLOG) < 0) {
        perror("listen");
        return 1;
    }

    printf("Motion CAPTCHA server listening on http://0.0.0.0:%d\n", PORT);

    while (1) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &client_len);
        if (client_fd < 0) continue;

        int *arg = malloc(sizeof(int));
        *arg = client_fd;
        pthread_t tid;
        if (pthread_create(&tid, NULL, handle_connection, arg) != 0) {
            close(client_fd);
            free(arg);
            continue;
        }
        pthread_detach(tid);
    }

    close(server_fd);
    return 0;
}
