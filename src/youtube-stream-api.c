#define _GNU_SOURCE
#include <curl/curl.h>
#include <json-c/json.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define YOUTUBE_API "https://www.googleapis.com/youtube/v3/"
#define YOUTUBE_UPLOAD_API "https://www.googleapis.com/upload/youtube/v3/"

typedef struct {
    char *data;
    size_t len;
} Buffer;

typedef struct {
    char *key;
    char *value;
} Param;

static char *config_dir;
static char *token_file;
static char *client_secret_file;

static void die(const char *fmt, ...) {
    va_list ap;
    fprintf(stderr, "youtube-stream-api: error: ");
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(1);
}

static char *xstrdup(const char *s) {
    if (!s) return NULL;
    char *out = strdup(s);
    if (!out) die("out of memory");
    return out;
}

static char *xasprintf(const char *fmt, ...) {
    va_list ap;
    char *out = NULL;
    va_start(ap, fmt);
    if (vasprintf(&out, fmt, ap) < 0) die("out of memory");
    va_end(ap);
    return out;
}

static const char *jstr(json_object *obj, const char *key) {
    json_object *v = NULL;
    if (!obj || !json_object_object_get_ex(obj, key, &v) || json_object_is_type(v, json_type_null)) return "";
    const char *s = json_object_get_string(v);
    return s ? s : "";
}

static json_object *jobj(json_object *obj, const char *key) {
    json_object *v = NULL;
    if (!obj || !json_object_object_get_ex(obj, key, &v) || !json_object_is_type(v, json_type_object)) return NULL;
    return v;
}

static json_object *jarr(json_object *obj, const char *key) {
    json_object *v = NULL;
    if (!obj || !json_object_object_get_ex(obj, key, &v) || !json_object_is_type(v, json_type_array)) return NULL;
    return v;
}

static void add_str(json_object *obj, const char *key, const char *value) {
    json_object_object_add(obj, key, json_object_new_string(value ? value : ""));
}

static void add_bool(json_object *obj, const char *key, bool value) {
    json_object_object_add(obj, key, json_object_new_boolean(value));
}

static int mkdir_p(const char *path, mode_t mode) {
    char *tmp = xstrdup(path);
    for (char *p = tmp + 1; *p; ++p) {
        if (*p == '/') {
            *p = 0;
            mkdir(tmp, mode);
            *p = '/';
        }
    }
    int rc = mkdir(tmp, mode);
    if (rc < 0 && errno == EEXIST) rc = 0;
    free(tmp);
    return rc;
}

static char *dirname_dup(const char *path) {
    char *copy = xstrdup(path);
    char *slash = strrchr(copy, '/');
    if (!slash) {
        free(copy);
        return xstrdup(".");
    }
    if (slash == copy) slash[1] = 0;
    else *slash = 0;
    return copy;
}

static char *read_file(const char *path, size_t *len_out) {
    FILE *f = fopen(path, "rb");
    if (!f) die("missing %s; create OAuth credentials and authorize first", path);
    if (fseek(f, 0, SEEK_END) != 0) die("failed to seek %s", path);
    long n = ftell(f);
    if (n < 0) die("failed to size %s", path);
    rewind(f);
    char *data = malloc((size_t)n + 1);
    if (!data) die("out of memory");
    if (fread(data, 1, (size_t)n, f) != (size_t)n) die("failed to read %s", path);
    fclose(f);
    data[n] = 0;
    if (len_out) *len_out = (size_t)n;
    return data;
}

static json_object *read_json_file(const char *path) {
    size_t len;
    char *data = read_file(path, &len);
    json_object *obj = json_tokener_parse(data);
    free(data);
    if (!obj) die("invalid JSON in %s", path);
    return obj;
}

static void write_json_atomic(const char *path, json_object *obj) {
    char *dir = dirname_dup(path);
    if (mkdir_p(dir, 0700) < 0) die("failed to create %s: %s", dir, strerror(errno));
    free(dir);
    char *tmp = xasprintf("%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) die("failed to write %s: %s", tmp, strerror(errno));
    const char *text = json_object_to_json_string_ext(obj, JSON_C_TO_STRING_PRETTY | JSON_C_TO_STRING_SPACED);
    fputs(text, f);
    fputc('\n', f);
    if (fclose(f) != 0) die("failed to close %s", tmp);
    chmod(tmp, 0600);
    if (rename(tmp, path) < 0) die("failed to replace %s: %s", path, strerror(errno));
    chmod(path, 0600);
    free(tmp);
}

static size_t write_cb(char *ptr, size_t size, size_t nmemb, void *userdata) {
    Buffer *b = userdata;
    size_t n = size * nmemb;
    char *p = realloc(b->data, b->len + n + 1);
    if (!p) return 0;
    b->data = p;
    memcpy(b->data + b->len, ptr, n);
    b->len += n;
    b->data[b->len] = 0;
    return n;
}

static char *build_query_url(const char *base, const char *path, Param *params, size_t count) {
    CURL *curl = curl_easy_init();
    if (!curl) die("failed to initialize curl");
    char *url = xasprintf("%s%s%s", base, path, count ? "?" : "");
    for (size_t i = 0; i < count; ++i) {
        char *ek = curl_easy_escape(curl, params[i].key, 0);
        char *ev = curl_easy_escape(curl, params[i].value, 0);
        char *old = url;
        url = xasprintf("%s%s%s=%s", old, i ? "&" : "", ek, ev);
        free(old);
        curl_free(ek);
        curl_free(ev);
    }
    curl_easy_cleanup(curl);
    return url;
}

static json_object *http_json_raw(const char *method, const char *url, const char *token, const char *content_type, const char *body, size_t body_len) {
    CURL *curl = curl_easy_init();
    if (!curl) die("failed to initialize curl");
    Buffer resp = {0};
    struct curl_slist *headers = NULL;
    if (token && *token) {
        char *auth = xasprintf("Authorization: Bearer %s", token);
        headers = curl_slist_append(headers, auth);
        free(auth);
    }
    if (content_type) {
        char *ct = xasprintf("Content-Type: %s", content_type);
        headers = curl_slist_append(headers, ct);
        free(ct);
    }
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    if (headers) curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    if (body) {
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)body_len);
    }
    CURLcode rc = curl_easy_perform(curl);
    long code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    if (rc != CURLE_OK) die("curl failed for %s: %s", url, curl_easy_strerror(rc));
    if (code < 200 || code >= 300) {
        const char *msg = resp.data ? resp.data : "";
        json_object *err = json_tokener_parse(msg);
        if (err) {
            json_object *e = jobj(err, "error");
            const char *m = jstr(e, "message");
            if (*m) msg = m;
        }
        die("HTTP %ld from %s: %s", code, url, msg);
    }
    if (!resp.data || resp.len == 0) return json_object_new_object();
    json_object *obj = json_tokener_parse(resp.data);
    if (!obj) die("invalid JSON from %s: %s", url, resp.data);
    free(resp.data);
    return obj;
}

static json_object *yt_get(const char *path, Param *params, size_t count, const char *token) {
    char *url = build_query_url(YOUTUBE_API, path, params, count);
    json_object *obj = http_json_raw("GET", url, token, NULL, NULL, 0);
    free(url);
    return obj;
}

static json_object *yt_json_method(const char *method, const char *path, Param *params, size_t count, const char *token, json_object *body_obj) {
    char *url = build_query_url(YOUTUBE_API, path, params, count);
    const char *body = json_object_to_json_string(body_obj);
    json_object *obj = http_json_raw(method, url, token, "application/json; charset=UTF-8", body, strlen(body));
    free(url);
    return obj;
}

static char *urlencode_form(Param *params, size_t count) {
    CURL *curl = curl_easy_init();
    if (!curl) die("failed to initialize curl");
    char *out = xstrdup("");
    for (size_t i = 0; i < count; ++i) {
        char *ek = curl_easy_escape(curl, params[i].key, 0);
        char *ev = curl_easy_escape(curl, params[i].value, 0);
        char *old = out;
        out = xasprintf("%s%s%s=%s", old, i ? "&" : "", ek, ev);
        free(old);
        curl_free(ek);
        curl_free(ev);
    }
    curl_easy_cleanup(curl);
    return out;
}

static const char *get_nested_client_value(json_object *client, const char *key) {
    if (!client) return "";
    json_object *installed = jobj(client, "installed");
    json_object *web = jobj(client, "web");
    const char *v = jstr(client, key);
    if (*v) return v;
    v = jstr(installed, key);
    if (*v) return v;
    return jstr(web, key);
}

static char *access_token(void) {
    json_object *token = read_json_file(token_file);
    json_object *client = NULL;
    const char *client_id = jstr(token, "client_id");
    const char *client_secret = jstr(token, "client_secret");
    if ((!*client_id || !*client_secret) && access(client_secret_file, R_OK) == 0) {
        client = read_json_file(client_secret_file);
        if (!*client_id) client_id = get_nested_client_value(client, "client_id");
        if (!*client_secret) client_secret = get_nested_client_value(client, "client_secret");
        if (*client_id) add_str(token, "client_id", client_id);
        if (*client_secret) add_str(token, "client_secret", client_secret);
    }
    const char *refresh = jstr(token, "refresh_token");
    if (!*client_id || !*client_secret) die("%s lacks OAuth client_id/client_secret and %s is missing them", token_file, client_secret_file);
    if (!*refresh) die("%s does not contain a refresh_token", token_file);

    time_t now = time(NULL);
    json_object *expires_obj = NULL;
    long long expires_at = 0;
    if (json_object_object_get_ex(token, "expires_at", &expires_obj)) expires_at = json_object_get_int64(expires_obj);
    const char *cached = jstr(token, "access_token");
    if (*cached && expires_at > (long long)now + 120) {
        char *out = xstrdup(cached);
        json_object_put(token);
        if (client) json_object_put(client);
        return out;
    }

    Param p[] = {
        {"client_id", (char *)client_id},
        {"client_secret", (char *)client_secret},
        {"refresh_token", (char *)refresh},
        {"grant_type", "refresh_token"},
    };
    char *body = urlencode_form(p, 4);
    json_object *refreshed = http_json_raw("POST", "https://oauth2.googleapis.com/token", NULL,
                                           "application/x-www-form-urlencoded", body, strlen(body));
    free(body);
    const char *new_access = jstr(refreshed, "access_token");
    if (!*new_access) die("OAuth refresh did not return access_token");
    add_str(token, "access_token", new_access);
    json_object *expires_in_obj = NULL;
    long expires_in = 3600;
    if (json_object_object_get_ex(refreshed, "expires_in", &expires_in_obj)) expires_in = json_object_get_int(expires_in_obj);
    json_object_object_add(token, "expires_at", json_object_new_int64((long long)now + expires_in));
    if (*client_id) add_str(token, "client_id", client_id);
    if (*client_secret) add_str(token, "client_secret", client_secret);
    write_json_atomic(token_file, token);
    char *out = xstrdup(new_access);
    json_object_put(refreshed);
    json_object_put(token);
    if (client) json_object_put(client);
    return out;
}

static void desired_stream_profile(char **resolution, char **frame_rate, char **title, char **protocol) {
    const char *res = getenv("YOUTUBE_STREAM_CDN_RESOLUTION");
    if (!res || !*res) res = "1080p";
    const char *fr = getenv("YOUTUBE_STREAM_CDN_FRAME_RATE");
    if (!fr || !*fr) fr = getenv("YOUTUBE_STREAM_CDN_FRAMERATE");
    if (!fr || !*fr) fr = "60fps";
    const char *proto = getenv("YOUTUBE_STREAM_INGEST_PROTOCOL");
    if (!proto || !*proto) proto = "rtmp";
    if (strcmp(proto, "rtmp") && strcmp(proto, "rtmps")) die("YOUTUBE_STREAM_INGEST_PROTOCOL must be rtmp or rtmps");
    const char *env_title = getenv("YOUTUBE_STREAM_LIVE_STREAM_TITLE");
    char *default_title = NULL;
    if (!env_title || !*env_title) {
        char *digits = xstrdup(fr);
        char *p = strstr(digits, "fps");
        if (p) *p = 0;
        default_title = xasprintf("youtube-stream %s%s", res, digits);
        free(digits);
        env_title = default_title;
    }
    *resolution = xstrdup(res);
    *frame_rate = xstrdup(fr);
    *title = xstrdup(env_title);
    *protocol = xstrdup(proto);
    free(default_title);
}

static char *stream_ingestion_url(json_object *stream, const char *protocol) {
    json_object *info = jobj(jobj(stream, "cdn"), "ingestionInfo");
    const char *stream_name = jstr(info, "streamName");
    if (!*stream_name) die("live stream %s has no ingestion streamName", jstr(stream, "id"));
    const char *address = "";
    if (!strcmp(protocol, "rtmps")) address = jstr(info, "rtmpsIngestionAddress");
    if (!*address) address = jstr(info, "ingestionAddress");
    if (!*address) die("live stream %s has no ingestion address", jstr(stream, "id"));
    size_t len = strlen(address);
    while (len > 0 && address[len - 1] == '/') len--;
    return xasprintf("%.*s/%s", (int)len, address, stream_name);
}

static json_object *choose_live_stream(const char *token) {
    char *resolution, *frame_rate, *title, *protocol;
    json_object *out = NULL;
    desired_stream_profile(&resolution, &frame_rate, &title, &protocol);
    const char *configured = getenv("YOUTUBE_STREAM_ID");
    if (!configured || !*configured) configured = getenv("YOUTUBE_STREAM_LIVE_STREAM_ID");
    if (configured && *configured) {
        Param p[] = {{"part", "id,snippet,cdn,status"}, {"id", (char *)configured}};
        json_object *resp = yt_get("liveStreams", p, 2, token);
        json_object *items = jarr(resp, "items");
        if (!items || json_object_array_length(items) == 0) die("configured live stream id not found: %s", configured);
        out = json_object_get(json_object_array_get_idx(items, 0));
        json_object_put(resp);
        goto done_with_out;
    }

    Param p[] = {{"part", "id,snippet,cdn,status"}, {"mine", "true"}, {"maxResults", "50"}};
    json_object *resp = yt_get("liveStreams", p, 3, token);
    json_object *items = jarr(resp, "items");
    json_object *best = NULL;
    if (items) {
        size_t n = json_object_array_length(items);
        for (size_t i = 0; i < n; ++i) {
            json_object *item = json_object_array_get_idx(items, i);
            json_object *cdn = jobj(item, "cdn");
            if (strcmp(jstr(cdn, "resolution"), resolution) || strcmp(jstr(cdn, "frameRate"), frame_rate)) continue;
            if (!best) {
                best = item;
                continue;
            }
            const char *item_title = jstr(jobj(item, "snippet"), "title");
            const char *best_title = jstr(jobj(best, "snippet"), "title");
            bool item_exact = !strcmp(item_title, title);
            bool best_exact = !strcmp(best_title, title);
            if (item_exact && !best_exact) best = item;
        }
    }
    if (best) {
        out = json_object_get(best);
        json_object_put(resp);
        goto done_with_out;
    }

    const char *create = getenv("YOUTUBE_STREAM_CREATE_LIVE_STREAM");
    if (create && (!*create || !strcmp(create, "0") || !strcasecmp(create, "no") || !strcasecmp(create, "false") || !strcasecmp(create, "off"))) {
        die("no YouTube Live stream key resource found for %s/%s", resolution, frame_rate);
    }
    json_object *body = json_object_new_object();
    json_object *snippet = json_object_new_object();
    add_str(snippet, "title", title);
    char *desc = xasprintf("Reusable stream key for youtube-stream %s %s", resolution, frame_rate);
    add_str(snippet, "description", desc);
    free(desc);
    json_object *cdn = json_object_new_object();
    add_str(cdn, "ingestionType", "rtmp");
    add_str(cdn, "resolution", resolution);
    add_str(cdn, "frameRate", frame_rate);
    json_object_object_add(body, "snippet", snippet);
    json_object_object_add(body, "cdn", cdn);
    Param cp[] = {{"part", "snippet,cdn"}};
    json_object *created = yt_json_method("POST", "liveStreams", cp, 1, token, body);
    json_object_put(body);
    json_object_put(resp);
    out = created;

done_with_out:
    free(resolution); free(frame_rate); free(title); free(protocol);
    return out;
}

static json_object *list_broadcasts(const char *token) {
    Param p[] = {{"part", "id,snippet,status,contentDetails"}, {"mine", "true"}, {"maxResults", "25"}};
    return yt_get("liveBroadcasts", p, 3, token);
}

static int broadcast_rank(const char *status) {
    if (!strcmp(status, "live")) return 0;
    if (!strcmp(status, "testing")) return 1;
    if (!strcmp(status, "ready")) return 2;
    if (!strcmp(status, "created")) return 3;
    if (!strcmp(status, "revoked")) return 8;
    if (!strcmp(status, "complete")) return 9;
    return 5;
}

static json_object *choose_broadcast(json_object *items, const char *requested_id) {
    if (!items) return NULL;
    size_t n = json_object_array_length(items);
    if (requested_id && *requested_id) {
        for (size_t i = 0; i < n; ++i) {
            json_object *item = json_object_array_get_idx(items, i);
            if (!strcmp(jstr(item, "id"), requested_id)) return json_object_get(item);
        }
        die("configured broadcast/video id not found: %s", requested_id);
    }
    json_object *best = NULL;
    int best_rank = 999;
    for (size_t i = 0; i < n; ++i) {
        json_object *item = json_object_array_get_idx(items, i);
        const char *life = jstr(jobj(item, "status"), "lifeCycleStatus");
        if (!strcmp(life, "complete")) continue;
        int rank = broadcast_rank(life);
        if (!best || rank < best_rank) {
            best = item;
            best_rank = rank;
        }
    }
    return best ? json_object_get(best) : NULL;
}

static json_object *create_broadcast(const char *token, const char *title, const char *description, const char *privacy, json_object *stream) {
    time_t t = time(NULL) + 15;
    struct tm tm;
    gmtime_r(&t, &tm);
    char scheduled[64];
    strftime(scheduled, sizeof scheduled, "%Y-%m-%dT%H:%M:%SZ", &tm);
    json_object *body = json_object_new_object();
    json_object *snippet = json_object_new_object();
    add_str(snippet, "title", title);
    add_str(snippet, "description", description);
    add_str(snippet, "scheduledStartTime", scheduled);
    json_object *status = json_object_new_object();
    add_str(status, "privacyStatus", privacy);
    add_bool(status, "selfDeclaredMadeForKids", false);
    json_object *content = json_object_new_object();
    add_bool(content, "enableAutoStart", true);
    add_bool(content, "enableAutoStop", true);
    add_bool(content, "enableDvr", true);
    const char *latency = getenv("YOUTUBE_STREAM_LATENCY");
    add_str(content, "latencyPreference", latency && *latency ? latency : "low");
    json_object *monitor = json_object_new_object();
    add_bool(monitor, "enableMonitorStream", true);
    json_object_object_add(content, "monitorStream", monitor);
    json_object_object_add(body, "snippet", snippet);
    json_object_object_add(body, "status", status);
    json_object_object_add(body, "contentDetails", content);
    Param p[] = {{"part", "snippet,status,contentDetails"}};
    json_object *broadcast = yt_json_method("POST", "liveBroadcasts", p, 1, token, body);
    json_object_put(body);
    json_object *empty = json_object_new_object();
    Param bp[] = {{"id", (char *)jstr(broadcast, "id")}, {"part", "id,snippet,status,contentDetails"}, {"streamId", (char *)jstr(stream, "id")}};
    json_object *bound = yt_json_method("POST", "liveBroadcasts/bind", bp, 3, token, empty);
    json_object_put(empty);
    json_object_put(broadcast);
    return bound;
}

static json_object *ensure_broadcast_bound(const char *token, json_object *broadcast, json_object *stream) {
    const char *current = jstr(jobj(broadcast, "contentDetails"), "boundStreamId");
    if (*current && !strcmp(current, jstr(stream, "id"))) return json_object_get(broadcast);
    const char *life = jstr(jobj(broadcast, "status"), "lifeCycleStatus");
    if (!strcmp(life, "live") || !strcmp(life, "testing")) {
        die("broadcast %s is %s but not bound to requested stream", jstr(broadcast, "id"), life);
    }
    json_object *empty = json_object_new_object();
    Param bp[] = {{"id", (char *)jstr(broadcast, "id")}, {"part", "id,snippet,status,contentDetails"}, {"streamId", (char *)jstr(stream, "id")}};
    json_object *bound = yt_json_method("POST", "liveBroadcasts/bind", bp, 3, token, empty);
    json_object_put(empty);
    return bound;
}

static const char *mime_for_path(const char *path) {
    const char *ext = strrchr(path, '.');
    if (!ext) return "application/octet-stream";
    if (!strcasecmp(ext, ".png")) return "image/png";
    if (!strcasecmp(ext, ".jpg") || !strcasecmp(ext, ".jpeg")) return "image/jpeg";
    return "application/octet-stream";
}

static json_object *upload_thumbnail(const char *token, const char *video_id, const char *file_path) {
    size_t file_len;
    char *file_data = read_file(file_path, &file_len);
    char boundary[96];
    snprintf(boundary, sizeof boundary, "===============%ld%ld==", (long)time(NULL), (long)getpid());
    const char *mime = mime_for_path(file_path);
    char *head1 = xasprintf("--%s\r\nContent-Type: application/json; charset=UTF-8\r\n\r\n{}\r\n", boundary);
    char *head2 = xasprintf("--%s\r\nContent-Type: %s\r\n\r\n", boundary, mime);
    char *tail = xasprintf("\r\n--%s--\r\n", boundary);
    size_t body_len = strlen(head1) + strlen(head2) + file_len + strlen(tail);
    char *body = malloc(body_len);
    if (!body) die("out of memory");
    size_t off = 0;
    memcpy(body + off, head1, strlen(head1)); off += strlen(head1);
    memcpy(body + off, head2, strlen(head2)); off += strlen(head2);
    memcpy(body + off, file_data, file_len); off += file_len;
    memcpy(body + off, tail, strlen(tail));
    char *ct = xasprintf("multipart/related; boundary=%s", boundary);
    Param p[] = {{"videoId", (char *)video_id}, {"uploadType", "multipart"}};
    char *url = build_query_url(YOUTUBE_UPLOAD_API, "thumbnails/set", p, 2);
    json_object *resp = http_json_raw("POST", url, token, ct, body, body_len);
    free(url); free(ct); free(body); free(file_data); free(head1); free(head2); free(tail);
    return resp;
}

static void command_update(int argc, char **argv) {
    const char *title = NULL, *description = NULL, *thumbnail = NULL, *privacy = NULL, *broadcast_id_arg = NULL;
    for (int i = 0; i < argc; ++i) {
        if (!strcmp(argv[i], "--title") && i + 1 < argc) title = argv[++i];
        else if (!strcmp(argv[i], "--description") && i + 1 < argc) description = argv[++i];
        else if (!strcmp(argv[i], "--thumbnail") && i + 1 < argc) thumbnail = argv[++i];
        else if (!strcmp(argv[i], "--privacy") && i + 1 < argc) privacy = argv[++i];
        else if (!strcmp(argv[i], "--broadcast-id") && i + 1 < argc) broadcast_id_arg = argv[++i];
        else die("unknown or incomplete update argument: %s", argv[i]);
    }
    if (!title || !description || !thumbnail) die("update requires --title, --description, and --thumbnail");
    bool privacy_requested = privacy && *privacy;
    const char *create_privacy = privacy_requested ? privacy : "private";
    char *token = access_token();
    json_object *stream = choose_live_stream(token);
    char *res, *fr, *stream_title, *protocol;
    desired_stream_profile(&res, &fr, &stream_title, &protocol);
    const char *requested = broadcast_id_arg;
    if (!requested || !*requested) requested = getenv("YOUTUBE_STREAM_BROADCAST_ID");
    if (!requested || !*requested) requested = getenv("YOUTUBE_STREAM_VIDEO_ID");
    json_object *list = list_broadcasts(token);
    json_object *items = jarr(list, "items");
    json_object *item = choose_broadcast(items, requested);
    bool created = false;
    if (!item) {
        if (requested && *requested) die("configured broadcast/video id is complete and cannot be reused: %s", requested);
        item = create_broadcast(token, title, description, create_privacy, stream);
        created = true;
    } else {
        json_object *bound = ensure_broadcast_bound(token, item, stream);
        json_object_put(item);
        item = bound;
    }
    const char *video_id = jstr(item, "id");
    json_object *snippet = json_object_new_object();
    json_object *old_snippet = jobj(item, "snippet");
    const char *scheduled = jstr(old_snippet, "scheduledStartTime");
    add_str(snippet, "title", title);
    add_str(snippet, "description", description);
    if (*scheduled) add_str(snippet, "scheduledStartTime", scheduled);
    json_object *body = json_object_new_object();
    add_str(body, "id", video_id);
    json_object_object_add(body, "snippet", snippet);
    char *part = xstrdup("snippet");
    if (privacy_requested || created) {
        json_object *st = json_object_new_object();
        add_str(st, "privacyStatus", create_privacy);
        add_bool(st, "selfDeclaredMadeForKids", false);
        json_object_object_add(body, "status", st);
        free(part);
        part = xstrdup("snippet,status");
    }
    Param up[] = {{"part", part}};
    json_object *updated = yt_json_method("PUT", "liveBroadcasts", up, 1, token, body);
    json_object *thumb = upload_thumbnail(token, video_id, thumbnail);
    char *ingest = stream_ingestion_url(stream, protocol);

    json_object *out = json_object_new_object();
    add_str(out, "broadcast_id", video_id);
    add_str(out, "video_id", video_id);
    char *watch = xasprintf("https://www.youtube.com/watch?v=%s", video_id);
    add_str(out, "watch_url", watch);
    add_str(out, "stream_id", jstr(stream, "id"));
    add_str(out, "stream_resolution", jstr(jobj(stream, "cdn"), "resolution"));
    add_str(out, "stream_frame_rate", jstr(jobj(stream, "cdn"), "frameRate"));
    add_str(out, "stream_url", ingest);
    add_str(out, "title", jstr(jobj(updated, "snippet"), "title"));
    add_str(out, "description", jstr(jobj(updated, "snippet"), "description"));
    add_str(out, "life_cycle_status", jstr(jobj(updated, "status"), "lifeCycleStatus"));
    add_str(out, "privacy_status", jstr(jobj(updated, "status"), "privacyStatus"));
    json_object *thumb_items = jarr(thumb, "items");
    add_bool(out, "thumbnail_updated", thumb_items && json_object_array_length(thumb_items) > 0);
    add_bool(out, "created_broadcast", created);
    printf("%s\n", json_object_to_json_string_ext(out, JSON_C_TO_STRING_PLAIN));

    free(watch); free(ingest); free(part); free(token); free(res); free(fr); free(stream_title); free(protocol);
    json_object_put(out); json_object_put(thumb); json_object_put(updated); json_object_put(body); json_object_put(item); json_object_put(list); json_object_put(stream);
}

static void command_complete(int argc, char **argv) {
    const char *id = NULL;
    for (int i = 0; i < argc; ++i) {
        if (!strcmp(argv[i], "--broadcast-id") && i + 1 < argc) id = argv[++i];
        else die("unknown or incomplete complete argument: %s", argv[i]);
    }
    if (!id) die("complete requires --broadcast-id");
    char *token = access_token();
    Param p[] = {{"part", "id,status"}, {"id", (char *)id}};
    json_object *resp = yt_get("liveBroadcasts", p, 2, token);
    json_object *items = jarr(resp, "items");
    if (!items || json_object_array_length(items) == 0) die("broadcast not found: %s", id);
    json_object *item = json_object_array_get_idx(items, 0);
    const char *life = jstr(jobj(item, "status"), "lifeCycleStatus");
    json_object *out = json_object_new_object();
    add_str(out, "broadcast_id", id);
    if (!strcmp(life, "complete") || !strcmp(life, "created") || !strcmp(life, "ready")) {
        add_str(out, "life_cycle_status", life);
        add_bool(out, "transitioned", false);
    } else {
        json_object *empty = json_object_new_object();
        Param tp[] = {{"id", (char *)id}, {"broadcastStatus", "complete"}, {"part", "id,status"}};
        json_object *tr = yt_json_method("POST", "liveBroadcasts/transition", tp, 3, token, empty);
        add_str(out, "broadcast_id", jstr(tr, "id"));
        add_str(out, "life_cycle_status", jstr(jobj(tr, "status"), "lifeCycleStatus"));
        add_bool(out, "transitioned", true);
        json_object_put(empty); json_object_put(tr);
    }
    printf("%s\n", json_object_to_json_string_ext(out, JSON_C_TO_STRING_PLAIN));
    json_object_put(out); json_object_put(resp); free(token);
}

static void command_status(int argc, char **argv) {
    const char *id = NULL;
    for (int i = 0; i < argc; ++i) {
        if (!strcmp(argv[i], "--broadcast-id") && i + 1 < argc) id = argv[++i];
        else die("unknown or incomplete status argument: %s", argv[i]);
    }
    if (!id) die("status requires --broadcast-id");
    char *token = access_token();
    Param bp[] = {{"part", "id,snippet,status"}, {"id", (char *)id}};
    json_object *broadcast_resp = yt_get("liveBroadcasts", bp, 2, token);
    json_object *bitems = jarr(broadcast_resp, "items");
    if (!bitems || json_object_array_length(bitems) == 0) die("broadcast not found: %s", id);
    json_object *broadcast = json_object_array_get_idx(bitems, 0);
    Param vp[] = {{"part", "id,snippet,status,liveStreamingDetails,statistics"}, {"id", (char *)id}};
    json_object *video_resp = yt_get("videos", vp, 2, token);
    json_object *vitems = jarr(video_resp, "items");
    json_object *video = (vitems && json_object_array_length(vitems)) ? json_object_array_get_idx(vitems, 0) : NULL;
    json_object *details = jobj(video, "liveStreamingDetails");
    json_object *stats = jobj(video, "statistics");
    json_object *out = json_object_new_object();
    add_str(out, "broadcast_id", jstr(broadcast, "id"));
    const char *title = jstr(jobj(broadcast, "snippet"), "title");
    if (!*title) title = jstr(jobj(video, "snippet"), "title");
    add_str(out, "title", title);
    add_str(out, "life_cycle_status", jstr(jobj(broadcast, "status"), "lifeCycleStatus"));
    const char *priv = jstr(jobj(broadcast, "status"), "privacyStatus");
    if (!*priv) priv = jstr(jobj(video, "status"), "privacyStatus");
    add_str(out, "privacy_status", priv);
    add_str(out, "actual_start_time", jstr(details, "actualStartTime"));
    add_str(out, "actual_end_time", jstr(details, "actualEndTime"));
    add_str(out, "concurrent_viewers", jstr(details, "concurrentViewers"));
    add_str(out, "view_count", jstr(stats, "viewCount"));
    char *watch = xasprintf("https://www.youtube.com/watch?v=%s", id);
    add_str(out, "watch_url", watch);
    printf("%s\n", json_object_to_json_string_ext(out, JSON_C_TO_STRING_PLAIN));
    free(watch); json_object_put(out); json_object_put(video_resp); json_object_put(broadcast_resp); free(token);
}

static void usage(FILE *out) {
    fprintf(out,
            "Usage: youtube-stream-api COMMAND [options]\n\n"
            "Commands:\n"
            "  update      Create/reuse/bind a 1080p60 stream, update metadata, print ingest JSON\n"
            "  complete    Transition a broadcast to complete when appropriate\n"
            "  status      Print broadcast/viewer status JSON\n");
}

int main(int argc, char **argv) {
    const char *xdg = getenv("XDG_CONFIG_HOME");
    const char *home = getenv("HOME");
    if (xdg && *xdg) config_dir = xasprintf("%s/youtube-stream", xdg);
    else if (home && *home) config_dir = xasprintf("%s/.config/youtube-stream", home);
    else die("HOME not set");
    token_file = xasprintf("%s/oauth-token.json", config_dir);
    client_secret_file = xasprintf("%s/client_secret.json", config_dir);
    curl_global_init(CURL_GLOBAL_DEFAULT);
    if (argc < 2) { usage(stderr); return 2; }
    if (!strcmp(argv[1], "-h") || !strcmp(argv[1], "--help")) { usage(stdout); return 0; }
    if (!strcmp(argv[1], "update")) command_update(argc - 2, argv + 2);
    else if (!strcmp(argv[1], "complete")) command_complete(argc - 2, argv + 2);
    else if (!strcmp(argv[1], "status")) command_status(argc - 2, argv + 2);
    else { usage(stderr); return 2; }
    curl_global_cleanup();
    free(config_dir); free(token_file); free(client_secret_file);
    return 0;
}
