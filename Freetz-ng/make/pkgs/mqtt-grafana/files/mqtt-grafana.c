#define _POSIX_C_SOURCE 200809L
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifndef DEFAULT_DB_PATH
#define DEFAULT_DB_PATH "/var/media/ftp/MQTTDATA/mqtt_messages.db"
#endif
#ifndef DEFAULT_MAX_ROWS
#define DEFAULT_MAX_ROWS 20000
#endif
#ifndef DEFAULT_ALLOWED_IP
#define DEFAULT_ALLOWED_IP ""
#endif
#ifndef DEFAULT_GRAFANA_TOPIC
#define DEFAULT_GRAFANA_TOPIC "OBK-681/power/get"
#endif

#define DEFAULT_LIMIT 2000
#define MAX_QUERY_VALUE 1024
#define MAX_TOPIC_LEN 768
#ifndef RUNTIME_CONFIG
#define RUNTIME_CONFIG "/mod/etc/conf/esp32c3.cfg"
#endif

static char runtime_db_path[512] = DEFAULT_DB_PATH;
static char runtime_allowed_ip[128] = DEFAULT_ALLOWED_IP;
static char runtime_grafana_topic[MAX_TOPIC_LEN + 1] = DEFAULT_GRAFANA_TOPIC;
static long long runtime_max_rows = DEFAULT_MAX_ROWS;

static char *trim(char *value)
{
    char *end;
    while (isspace((unsigned char)*value)) ++value;
    end = value + strlen(value);
    while (end > value && isspace((unsigned char)end[-1])) --end;
    *end = '\0';
    return value;
}

static void copy_config_value(char *dst, size_t dst_size, char *value)
{
    size_t len;
    value = trim(value);
    len = strlen(value);
    if (len >= 2 && ((value[0] == '\'' && value[len - 1] == '\'') ||
                     (value[0] == '"' && value[len - 1] == '"'))) {
        value[len - 1] = '\0';
        ++value;
    }
    snprintf(dst, dst_size, "%s", value);
}

static void load_runtime_config(void)
{
    FILE *file = fopen(RUNTIME_CONFIG, "r");
    char line[1024];
    if (!file) return;

    while (fgets(line, sizeof(line), file)) {
        char *key = trim(line), *value, *equals;
        char *end = NULL;
        long long parsed;
        if (!*key || *key == '#') continue;
        equals = strchr(key, '=');
        if (!equals) continue;
        *equals = '\0';
        value = equals + 1;
        key = trim(key);

        if (!strcmp(key, "MQTT_DB_PATH")) {
            copy_config_value(runtime_db_path, sizeof(runtime_db_path), value);
        } else if (!strcmp(key, "GRAFANA_TOPIC")) {
            copy_config_value(runtime_grafana_topic, sizeof(runtime_grafana_topic), value);
        } else if (!strcmp(key, "GRAFANA_ALLOWED_IP")) {
            copy_config_value(runtime_allowed_ip, sizeof(runtime_allowed_ip), value);
        } else if (!strcmp(key, "GRAFANA_MAX_ROWS")) {
            char number[32];
            copy_config_value(number, sizeof(number), value);
            errno = 0;
            parsed = strtoll(number, &end, 10);
            if (!errno && end != number && *trim(end) == '\0' &&
                parsed >= 1 && parsed <= 100000) {
                runtime_max_rows = parsed;
            }
        }
    }
    fclose(file);
}

static void json_string(const unsigned char *value)
{
    const unsigned char *p;
    if (value == NULL) { fputs("null", stdout); return; }
    putchar('"');
    for (p = value; *p; ++p) {
        switch (*p) {
        case '"': fputs("\\\"", stdout); break;
        case '\\': fputs("\\\\", stdout); break;
        case '\b': fputs("\\b", stdout); break;
        case '\f': fputs("\\f", stdout); break;
        case '\n': fputs("\\n", stdout); break;
        case '\r': fputs("\\r", stdout); break;
        case '\t': fputs("\\t", stdout); break;
        default:
            if (*p < 0x20) printf("\\u%04x", (unsigned int)*p);
            else putchar((int)*p);
        }
    }
    putchar('"');
}

static void headers(void)
{
    fputs("Content-Type: application/json; charset=utf-8\r\n", stdout);
    fputs("Cache-Control: no-store\r\n", stdout);
    fputs("X-Content-Type-Options: nosniff\r\n", stdout);
    fputs("\r\n", stdout);
}

static void error_response(int status, const char *text, const char *message)
{
    printf("Status: %d %s\r\n", status, text);
    headers();
    fputs("{\"error\":", stdout);
    json_string((const unsigned char *)message);
    fputs("}\n", stdout);
}

static int config_response(void)
{
    if (!runtime_grafana_topic[0]) {
        error_response(503, "Service Unavailable", "No dashboard topic is configured");
        return 1;
    }
    if (strchr(runtime_grafana_topic, '+') || strchr(runtime_grafana_topic, '#')) {
        error_response(503, "Service Unavailable", "Dashboard topic must not contain MQTT wildcards");
        return 1;
    }
    headers();
    fputs("{\"topic\":", stdout);
    json_string((const unsigned char *)runtime_grafana_topic);
    printf(",\"max_rows\":%lld}\n", runtime_max_rows);
    return 0;
}

static int hex_value(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int url_decode(const char *src, size_t src_len, char *dst, size_t dst_size)
{
    size_t i = 0, used = 0;
    while (i < src_len) {
        unsigned char c;
        if (used + 1 >= dst_size) return -1;
        if (src[i] == '+') { c = ' '; ++i; }
        else if (src[i] == '%') {
            int hi, lo;
            if (i + 2 >= src_len) return -1;
            hi = hex_value(src[i + 1]); lo = hex_value(src[i + 2]);
            if (hi < 0 || lo < 0) return -1;
            c = (unsigned char)((hi << 4) | lo); i += 3;
        } else c = (unsigned char)src[i++];
        if (c == '\0') return -1;
        dst[used++] = (char)c;
    }
    dst[used] = '\0';
    return 0;
}

static int query_value(const char *query, const char *name, char *out, size_t out_size)
{
    size_t name_len = strlen(name);
    const char *part = query;
    if (!query || !*query) return 0;
    while (*part) {
        const char *end = strchr(part, '&');
        size_t part_len = end ? (size_t)(end - part) : strlen(part);
        if (part_len > name_len && !strncmp(part, name, name_len) && part[name_len] == '=') {
            if (url_decode(part + name_len + 1, part_len - name_len - 1, out, out_size)) return -1;
            return 1;
        }
        if (!end) break;
        part = end + 1;
    }
    return 0;
}

static long long parse_int(const char *s, long long fallback, long long min, long long max, int *ok)
{
    char *end = NULL;
    long long v;
    if (!s || !*s) { *ok = 1; return fallback; }
    errno = 0; v = strtoll(s, &end, 10);
    if (errno || end == s || *end || v < min || v > max) { *ok = 0; return fallback; }
    *ok = 1; return v;
}

static long long normalize_epoch(long long value)
{
    return value > 100000000000LL ? value / 1000LL : value;
}

static int numeric_payload(const char *payload, double *number)
{
    char *end = NULL;
    double v;
    if (!payload) return 0;
    while (isspace((unsigned char)*payload)) ++payload;
    if (!*payload) return 0;
    errno = 0; v = strtod(payload, &end);
    if (errno || end == payload || !isfinite(v)) return 0;
    while (isspace((unsigned char)*end)) ++end;
    if (*end) return 0;
    *number = v; return 1;
}

static int client_allowed(void)
{
    const char *remote;
    if (!runtime_allowed_ip[0]) return 1;
    remote = getenv("REMOTE_ADDR");
    return remote && !strcmp(remote, runtime_allowed_ip);
}

static int open_database(sqlite3 **db)
{
    int rc = sqlite3_open_v2(runtime_db_path, db,
                             SQLITE_OPEN_READONLY | SQLITE_OPEN_NOMUTEX, NULL);
    if (rc == SQLITE_OK) sqlite3_busy_timeout(*db, 2000);
    return rc;
}

static int health_response(void)
{
    sqlite3 *db = NULL;
    sqlite3_stmt *stmt = NULL;
    sqlite3_int64 count = 0, latest = 0;
    int rc = open_database(&db);
    if (rc != SQLITE_OK) {
        error_response(503, "Service Unavailable", db ? sqlite3_errmsg(db) : "Cannot open database");
        if (db) sqlite3_close(db);
        return 1;
    }
    rc = sqlite3_prepare_v2(db, "SELECT count(*), coalesce(max(ts),0) FROM messages", -1, &stmt, NULL);
    if (rc == SQLITE_OK && sqlite3_step(stmt) == SQLITE_ROW) {
        count = sqlite3_column_int64(stmt, 0); latest = sqlite3_column_int64(stmt, 1);
    } else {
        error_response(500, "Internal Server Error", sqlite3_errmsg(db));
        if (stmt) sqlite3_finalize(stmt);
        sqlite3_close(db);
        return 1;
    }
    headers();
    printf("{\"status\":\"ok\",\"rows\":%lld,\"latest_time\":%lld}\n",
           (long long)count, (long long)latest * 1000LL);
    sqlite3_finalize(stmt); sqlite3_close(db); return 0;
}

int main(void)
{
    const char *query = getenv("QUERY_STRING");
    char from_s[32] = "", to_s[32] = "", limit_s[32] = "";
    char topic[MAX_TOPIC_LEN + 1] = "", mode[32] = "";
    long long now = (long long)time(NULL), from, to, limit;
    int ok, has_topic, rc, first = 1;
    sqlite3 *db = NULL; sqlite3_stmt *stmt = NULL;
    const char *sql_topic = "SELECT ts,topic,payload,qos,retain FROM messages WHERE ts>=?1 AND ts<=?2 AND topic=?3 ORDER BY ts ASC LIMIT ?4";
    const char *sql_all = "SELECT ts,topic,payload,qos,retain FROM messages WHERE ts>=?1 AND ts<=?2 ORDER BY ts ASC LIMIT ?3";

    load_runtime_config();
    if (!client_allowed()) { error_response(403, "Forbidden", "Client address is not permitted"); return 1; }
    if (query_value(query, "mode", mode, sizeof(mode)) < 0) { error_response(400, "Bad Request", "Invalid mode"); return 1; }
    if (!strcmp(mode, "config")) return config_response();
    if (!strcmp(mode, "health")) return health_response();
    if (query_value(query, "from", from_s, sizeof(from_s)) < 0 ||
        query_value(query, "to", to_s, sizeof(to_s)) < 0 ||
        query_value(query, "limit", limit_s, sizeof(limit_s)) < 0) {
        error_response(400, "Bad Request", "Invalid query parameter"); return 1;
    }
    has_topic = query_value(query, "topic", topic, sizeof(topic));
    if (has_topic < 0) { error_response(400, "Bad Request", "Invalid topic"); return 1; }
    from = parse_int(from_s, now - 3600, 0, LLONG_MAX / 1000, &ok);
    if (!ok) { error_response(400, "Bad Request", "Invalid from"); return 1; }
    to = parse_int(to_s, now, 0, LLONG_MAX / 1000, &ok);
    if (!ok) { error_response(400, "Bad Request", "Invalid to"); return 1; }
    from = normalize_epoch(from); to = normalize_epoch(to);
    limit = parse_int(limit_s, DEFAULT_LIMIT, 1, runtime_max_rows, &ok);
    if (!ok) { error_response(400, "Bad Request", "Invalid limit"); return 1; }
    if (from > to) { error_response(400, "Bad Request", "from must be <= to"); return 1; }

    rc = open_database(&db);
    if (rc != SQLITE_OK) {
        error_response(503, "Service Unavailable", db ? sqlite3_errmsg(db) : "Cannot open database");
        if (db) sqlite3_close(db);
        return 1;
    }
    rc = sqlite3_prepare_v2(db, has_topic ? sql_topic : sql_all, -1, &stmt, NULL);
    if (rc != SQLITE_OK) { error_response(500, "Internal Server Error", sqlite3_errmsg(db)); sqlite3_close(db); return 1; }
    sqlite3_bind_int64(stmt, 1, from); sqlite3_bind_int64(stmt, 2, to);
    if (has_topic) { sqlite3_bind_text(stmt, 3, topic, -1, SQLITE_TRANSIENT); sqlite3_bind_int64(stmt, 4, limit); }
    else sqlite3_bind_int64(stmt, 3, limit);

    headers(); putchar('[');
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        sqlite3_int64 ts = sqlite3_column_int64(stmt, 0);
        const unsigned char *row_topic = sqlite3_column_text(stmt, 1);
        const unsigned char *payload = sqlite3_column_text(stmt, 2);
        double number = 0.0; int is_number = numeric_payload((const char *)payload, &number);
        if (!first) putchar(',');
        first = 0;
        printf("{\"time\":%lld,\"topic\":", (long long)ts * 1000LL); json_string(row_topic);
        fputs(",\"value\":", stdout); if (is_number) printf("%.17g", number); else fputs("null", stdout);
        fputs(",\"payload\":", stdout); json_string(payload);
        printf(",\"qos\":%d,\"retain\":%d}", sqlite3_column_int(stmt, 3), sqlite3_column_int(stmt, 4));
    }
    putchar(']'); putchar('\n');
    if (rc != SQLITE_DONE) { /* response already started; signal failure to server log */ }
    sqlite3_finalize(stmt); sqlite3_close(db);
    return rc == SQLITE_DONE ? 0 : 1;
}
