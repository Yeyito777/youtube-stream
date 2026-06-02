#define _GNU_SOURCE
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <json-c/json.h>
#include <limits.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/termios.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define DEFAULT_FPS 60
#define DEFAULT_VIDEO_BITRATE_KBPS 12000
#define DEFAULT_AUDIO_BITRATE "192k"
#define DEFAULT_CAPTURE_BACKEND "gsr-direct-rtmp"
#define DEFAULT_CDN_RESOLUTION "1080p"
#define DEFAULT_CDN_FRAME_RATE "60fps"
#define DEFAULT_GSR_TUNE "quality"
#define DEFAULT_GPU_PERF "high"
#define DEFAULT_MIC_SOURCE "default_input"
#define DEFAULT_SYSTEM_SOURCE "default_output"
#define DEFAULT_MIC_GAIN 10.5
#define DEFAULT_SYSTEM_GAIN 0.675

/* Exocortex whale theme colors (~/Workspace/exocortex/tui/src/themes/whale.ts). */
#define W_RESET        "\033[0m"
#define W_BOLD         "\033[1m"
#define W_ACCENT       "\033[38;2;29;155;240m"
#define W_TEXT         "\033[38;2;255;255;255m"
#define W_MUTED        "\033[38;2;100;100;100m"
#define W_SUCCESS      "\033[38;2;80;200;120m"
#define W_WARNING      "\033[33m"
#define W_ERROR        "\033[31m"
#define W_COMMAND      "\033[38;2;174;214;254m"
#define W_NORMAL       "\033[38;2;72;202;228m"
#define W_TOPBAR_BG    "\033[48;2;29;155;240m"
#define W_APP_BG       "\033[48;2;0;5;15m"
#define W_SELECTION_BG "\033[48;2;79;82;88m"

typedef struct {
    char *key;
    char *value;
} Kv;

typedef struct {
    Kv *items;
    size_t len;
    size_t cap;
} Config;

typedef struct {
    char **v;
    size_t len;
    size_t cap;
} Argv;

typedef struct {
    char *data;
    size_t len;
} Buffer;

typedef struct {
    char *config_dir;
    char *config_file;
    char *state_dir;
    char *program_dir;
    char *api_helper;
    char *outline_helper;

    char *title;
    char *description;
    char *thumbnail;
    char *privacy;

    int start_browser;
    int start_outline;
    int start_audio;
    int start_tui;
    int dry_run;
    int setup_only;

    char *backend;
    int fps;
    int video_bitrate;
    char *video_codec;
    int keyint;
    char *encoder;
    char *fallback_cpu;
    char *gsr_tune;
    char *audio_bitrate;
    char *mic_source_raw;
    char *system_source_raw;
    char *gsr_audio_source;
    char *gsr_system_app;
    char *gsr_mic_app;
    double mic_gain;
    double system_gain;
    int mic_gain_percent;
    int system_gain_percent;

    char *stream_url;
    char *watch_url;
    char *broadcast_id;
    char *redacted_url;

    char monitor[128];
    int capture_w;
    int capture_h;
    int capture_x;
    int capture_y;

    Argv cmd;
    pid_t stream_pid;
    pid_t outline_pid;
    char *log_file;

    char *gpu_perf_path;
    char *gpu_perf_old;
    int gpu_perf_set;
    int stream_completed;

    struct termios old_termios;
    int termios_saved;
    int tui_selected;
    char tui_status[64];
    char tui_viewers[64];
    char tui_status_updated[64];
    char tui_message[256];
} App;

static volatile sig_atomic_t interrupted = 0;
static App *global_app = NULL;

static void on_signal(int sig) {
    (void)sig;
    interrupted = 1;
    if (global_app && global_app->stream_pid > 0) kill(global_app->stream_pid, SIGINT);
}

static void die(const char *fmt, ...) {
    va_list ap;
    fprintf(stderr, "youtube-stream: error: ");
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(1);
}

static void warnx(const char *fmt, ...) {
    va_list ap;
    fprintf(stderr, "youtube-stream: warning: ");
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

static void logx(const char *fmt, ...) {
    va_list ap;
    fprintf(stderr, "youtube-stream: ");
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
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

static bool streq(const char *a, const char *b) { return a && b && strcmp(a, b) == 0; }

static const char *nonnull(const char *s) { return s ? s : ""; }

static void copy_string(char *dst, size_t dst_len, const char *src) {
    if (!dst || dst_len == 0) return;
    snprintf(dst, dst_len, "%s", src ? src : "");
}

static char *path_join(const char *a, const char *b) {
    if (!a || !*a) return xstrdup(b);
    if (a[strlen(a) - 1] == '/') return xasprintf("%s%s", a, b);
    return xasprintf("%s/%s", a, b);
}

static char *dirname_dup(const char *path) {
    char *copy = xstrdup(path ? path : ".");
    char *slash = strrchr(copy, '/');
    if (!slash) { free(copy); return xstrdup("."); }
    if (slash == copy) slash[1] = 0;
    else *slash = 0;
    return copy;
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

static bool command_exists(const char *name) {
    if (!name || !*name) return false;
    if (strchr(name, '/')) return access(name, X_OK) == 0;
    const char *path = getenv("PATH");
    if (!path) return false;
    char *copy = xstrdup(path);
    bool found = false;
    for (char *save = NULL, *dir = strtok_r(copy, ":", &save); dir; dir = strtok_r(NULL, ":", &save)) {
        char *candidate = xasprintf("%s/%s", *dir ? dir : ".", name);
        if (access(candidate, X_OK) == 0) { found = true; free(candidate); break; }
        free(candidate);
    }
    free(copy);
    return found;
}

static void require_command(const char *name) {
    if (!command_exists(name)) die("missing dependency: %s", name);
}

static void argv_init(Argv *a) { memset(a, 0, sizeof *a); }

static void argv_push(Argv *a, const char *s) {
    if (a->len + 1 >= a->cap) {
        a->cap = a->cap ? a->cap * 2 : 32;
        a->v = realloc(a->v, a->cap * sizeof(char *));
        if (!a->v) die("out of memory");
    }
    a->v[a->len++] = xstrdup(s);
    a->v[a->len] = NULL;
}

static void argv_pushf(Argv *a, const char *fmt, ...) {
    va_list ap;
    char *s = NULL;
    va_start(ap, fmt);
    if (vasprintf(&s, fmt, ap) < 0) die("out of memory");
    va_end(ap);
    argv_push(a, s);
    free(s);
}

static void argv_free(Argv *a) {
    for (size_t i = 0; i < a->len; ++i) free(a->v[i]);
    free(a->v);
    memset(a, 0, sizeof *a);
}

static void shell_quote_print(const char *s) {
    if (!s || !*s) { printf("''"); return; }
    bool safe = true;
    for (const unsigned char *p = (const unsigned char *)s; *p; ++p) {
        if (!(isalnum(*p) || strchr("_@%+=:,./-", *p))) { safe = false; break; }
    }
    if (safe) { printf("%s", s); return; }
    putchar('\'');
    for (const char *p = s; *p; ++p) {
        if (*p == '\'') printf("'\\''");
        else putchar(*p);
    }
    putchar('\'');
}

static char *redact_url(const char *url) {
    if (!url) return xstrdup("<stream-url-redacted>");
    const char *live = strstr(url, "/live2/");
    if (!live) return xstrdup("<stream-url-redacted>");
    size_t prefix_len = (size_t)(live - url) + strlen("/live2/");
    return xasprintf("%.*s<stream-key-redacted>", (int)prefix_len, url);
}

static void print_command(const char *label, Argv *cmd, const char *stream_url) {
    printf("%s: ", label);
    for (size_t i = 0; i < cmd->len; ++i) {
        if (stream_url && streq(cmd->v[i], stream_url)) {
            char *r = redact_url(stream_url);
            shell_quote_print(r);
            free(r);
        } else {
            shell_quote_print(cmd->v[i]);
        }
        putchar(' ');
    }
    putchar('\n');
}

static char *trim(char *s) {
    while (isspace((unsigned char)*s)) s++;
    char *end = s + strlen(s);
    while (end > s && isspace((unsigned char)end[-1])) *--end = 0;
    return s;
}

static char *unquote_value(char *v) {
    v = trim(v);
    size_t len = strlen(v);
    if (len >= 2 && ((v[0] == '\'' && v[len - 1] == '\'') || (v[0] == '"' && v[len - 1] == '"'))) {
        v[len - 1] = 0;
        return xstrdup(v + 1);
    }
    return xstrdup(v);
}

static void config_add(Config *c, const char *key, const char *value) {
    if (c->len == c->cap) {
        c->cap = c->cap ? c->cap * 2 : 32;
        c->items = realloc(c->items, c->cap * sizeof(Kv));
        if (!c->items) die("out of memory");
    }
    c->items[c->len].key = xstrdup(key);
    c->items[c->len].value = xstrdup(value);
    c->len++;
}

static void load_config(Config *c, const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return;
    char *line = NULL;
    size_t cap = 0;
    while (getline(&line, &cap, f) >= 0) {
        char *s = trim(line);
        if (!*s || *s == '#') continue;
        if (!strncmp(s, "export", 6) && isspace((unsigned char)s[6])) s = trim(s + 6);
        char *eq = strchr(s, '=');
        if (!eq) continue;
        *eq = 0;
        char *key = trim(s);
        char *hash = strchr(eq + 1, '#');
        if (hash) *hash = 0;
        char *value = unquote_value(eq + 1);
        config_add(c, key, value);
        free(value);
    }
    free(line);
    fclose(f);
}

static const char *config_get(Config *c, const char *key) {
    const char *env = getenv(key);
    if (env) return env;
    for (size_t i = c->len; i > 0; --i) {
        if (!strcmp(c->items[i - 1].key, key)) return c->items[i - 1].value;
    }
    return NULL;
}

static const char *config_get_default(Config *c, const char *key, const char *def) {
    const char *v = config_get(c, key);
    return (v && *v) ? v : def;
}

static int config_get_int(Config *c, const char *key, int def) {
    const char *v = config_get(c, key);
    return (v && *v) ? atoi(v) : def;
}

static double config_get_double(Config *c, const char *key, double def) {
    const char *v = config_get(c, key);
    return (v && *v) ? atof(v) : def;
}

static bool falseish(const char *v) {
    return v && (!*v || !strcasecmp(v, "0") || !strcasecmp(v, "no") || !strcasecmp(v, "false") || !strcasecmp(v, "off"));
}

static char *to_gsr_audio_source(const char *raw) {
    if (!raw || !*raw) return xstrdup("");
    if (!strcmp(raw, "@DEFAULT_SOURCE@")) return xstrdup("default_input");
    if (!strcmp(raw, "@DEFAULT_MONITOR@")) return xstrdup("default_output");
    if (!strcmp(raw, "default_input") || !strcmp(raw, "default_output")) return xstrdup(raw);
    if (!strncmp(raw, "device:", 7)) return xstrdup(raw + 7);
    return xstrdup(raw);
}

static char *strip_k_suffix(const char *s) {
    char *out = xstrdup(s ? s : "192k");
    size_t len = strlen(out);
    if (len && (out[len - 1] == 'k' || out[len - 1] == 'K')) out[len - 1] = 0;
    return out;
}

static const char *jstr(json_object *obj, const char *key) {
    json_object *v = NULL;
    if (!obj || !json_object_object_get_ex(obj, key, &v) || json_object_is_type(v, json_type_null)) return "";
    const char *s = json_object_get_string(v);
    return s ? s : "";
}

static char *run_capture(char *const argv[], int *status_out) {
    int pipefd[2];
    if (pipe(pipefd) < 0) die("pipe failed: %s", strerror(errno));
    pid_t pid = fork();
    if (pid < 0) die("fork failed: %s", strerror(errno));
    if (pid == 0) {
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[1]);
        execvp(argv[0], argv);
        fprintf(stderr, "failed to exec %s: %s\n", argv[0], strerror(errno));
        _exit(127);
    }
    close(pipefd[1]);
    Buffer b = {0};
    char tmp[4096];
    ssize_t n;
    while ((n = read(pipefd[0], tmp, sizeof tmp)) > 0) {
        char *p = realloc(b.data, b.len + (size_t)n + 1);
        if (!p) die("out of memory");
        b.data = p;
        memcpy(b.data + b.len, tmp, (size_t)n);
        b.len += (size_t)n;
        b.data[b.len] = 0;
    }
    close(pipefd[0]);
    int status = 0;
    waitpid(pid, &status, 0);
    if (status_out) *status_out = status;
    if (!b.data) b.data = xstrdup("");
    return b.data;
}

static char *find_executable_in_path(const char *name) {
    if (!name || !*name) return NULL;
    if (strchr(name, '/')) return access(name, X_OK) == 0 ? xstrdup(name) : NULL;
    const char *path = getenv("PATH");
    if (!path) return NULL;
    char *copy = xstrdup(path);
    for (char *save = NULL, *dir = strtok_r(copy, ":", &save); dir; dir = strtok_r(NULL, ":", &save)) {
        char *candidate = xasprintf("%s/%s", *dir ? dir : ".", name);
        if (access(candidate, X_OK) == 0) { free(copy); return candidate; }
        free(candidate);
    }
    free(copy);
    return NULL;
}

static char *find_api_helper(App *app, Config *cfg) {
    const char *env = config_get(cfg, "YOUTUBE_STREAM_API_HELPER");
    if (env && *env && access(env, X_OK) == 0) return xstrdup(env);
    char *candidates[5];
    candidates[0] = path_join(app->program_dir, "youtube-stream-api");
    candidates[1] = xasprintf("%s/../lib/youtube-stream/youtube-stream-api", app->program_dir);
    const char *home = getenv("HOME");
    candidates[2] = home ? xasprintf("%s/.local/lib/youtube-stream/youtube-stream-api", home) : NULL;
    candidates[3] = xstrdup("build/youtube-stream-api");
    candidates[4] = NULL;
    for (int i = 0; candidates[i]; ++i) {
        if (access(candidates[i], X_OK) == 0) {
            char *out = xstrdup(candidates[i]);
            for (int j = 0; candidates[j]; ++j) free(candidates[j]);
            return out;
        }
    }
    for (int j = 0; candidates[j]; ++j) free(candidates[j]);
    return NULL;
}

static char *find_outline_helper(App *app) {
    char *candidates[4];
    candidates[0] = path_join(app->program_dir, "youtube-stream-outline");
    candidates[1] = xasprintf("%s/../lib/youtube-stream/youtube-stream-outline", app->program_dir);
    const char *home = getenv("HOME");
    candidates[2] = home ? xasprintf("%s/.local/lib/youtube-stream/youtube-stream-outline", home) : NULL;
    candidates[3] = NULL;
    for (int i = 0; candidates[i]; ++i) {
        if (access(candidates[i], X_OK) == 0) {
            char *out = xstrdup(candidates[i]);
            for (int j = 0; candidates[j]; ++j) free(candidates[j]);
            return out;
        }
    }
    for (int j = 0; candidates[j]; ++j) free(candidates[j]);
    return NULL;
}

static void set_env_defaults(App *app) {
    setenv("YOUTUBE_STREAM_CDN_RESOLUTION", getenv("YOUTUBE_STREAM_CDN_RESOLUTION") ?: DEFAULT_CDN_RESOLUTION, 0);
    if (!getenv("YOUTUBE_STREAM_CDN_FRAME_RATE") && !getenv("YOUTUBE_STREAM_CDN_FRAMERATE")) {
        setenv("YOUTUBE_STREAM_CDN_FRAME_RATE", app->fps >= 50 ? "60fps" : "30fps", 1);
    }
    setenv("YOUTUBE_STREAM_CREATE_LIVE_STREAM", getenv("YOUTUBE_STREAM_CREATE_LIVE_STREAM") ?: "yes", 0);
}

static void apply_youtube_metadata_api(App *app) {
    char *argv[] = {
        app->api_helper,
        "update",
        "--title", app->title,
        "--description", app->description,
        "--thumbnail", app->thumbnail,
        app->privacy && *app->privacy ? "--privacy" : NULL,
        app->privacy && *app->privacy ? app->privacy : NULL,
        NULL
    };
    char *argv_no_priv[] = {
        app->api_helper,
        "update",
        "--title", app->title,
        "--description", app->description,
        "--thumbnail", app->thumbnail,
        NULL
    };
    int status = 0;
    char *out = run_capture((app->privacy && *app->privacy) ? argv : argv_no_priv, &status);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) die("failed to update YouTube metadata: %s", out);
    json_object *obj = json_tokener_parse(out);
    if (!obj) die("invalid API output: %s", out);
    app->watch_url = xstrdup(jstr(obj, "watch_url"));
    app->stream_url = xstrdup(jstr(obj, "stream_url"));
    app->broadcast_id = xstrdup(jstr(obj, "broadcast_id"));
    logx("updated YouTube stream metadata through API");
    if (app->watch_url && *app->watch_url) {
        logx("stream page: %s", app->watch_url);
        char *watch_path = path_join(app->state_dir, "current-watch-url");
        FILE *f = fopen(watch_path, "w");
        if (f) { fprintf(f, "%s\n", app->watch_url); fclose(f); }
        free(watch_path);
        if (app->start_browser) {
            const char *browser_override = getenv("VIMBROWSER_CLI");
            char *vimbrowser = (browser_override && *browser_override)
                ? xstrdup(browser_override)
                : find_executable_in_path("vimbrowser-cli");
            if (vimbrowser) {
                pid_t pid = fork();
                if (pid == 0) { execl(vimbrowser, vimbrowser, "open", app->watch_url, (char *)NULL); _exit(127); }
                if (pid > 0) waitpid(pid, NULL, 0);
                free(vimbrowser);
            }
        }
    }
    json_object_put(obj);
    free(out);
}

static bool parse_geometry_token(const char *tok, int *w, int *h, int *x, int *y) {
    char sx, sy;
    int n = 0;
    if (sscanf(tok, "%dx%d%c%d%c%d%n", w, h, &sx, x, &sy, y, &n) == 6 && tok[n] == 0 && (sx == '+' || sx == '-') && (sy == '+' || sy == '-')) {
        if (sx == '-') *x = -*x;
        if (sy == '-') *y = -*y;
        return true;
    }
    return false;
}

static bool parse_xrandr_line(const char *line, char *name, size_t name_len, int *w, int *h, int *x, int *y) {
    const char *conn = strstr(line, " connected");
    if (!conn) return false;
    size_t nlen = (size_t)(conn - line);
    if (nlen >= name_len) nlen = name_len - 1;
    memcpy(name, line, nlen);
    name[nlen] = 0;
    char *copy = xstrdup(line);
    bool ok = false;
    for (char *save = NULL, *tok = strtok_r(copy, " \t\n", &save); tok; tok = strtok_r(NULL, " \t\n", &save)) {
        if (parse_geometry_token(tok, w, h, x, y)) { ok = true; break; }
    }
    free(copy);
    return ok;
}

static void get_primary_monitor(App *app) {
    char *argv[] = {"xrandr", "--query", NULL};
    int status = 0;
    char *out = run_capture(argv, &status);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) die("xrandr failed: %s", out);
    char first_name[128] = "";
    int fw = 0, fh = 0, fx = 0, fy = 0;
    char *save = NULL;
    for (char *line = strtok_r(out, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        char name[128];
        int w, h, x, y;
        if (!parse_xrandr_line(line, name, sizeof name, &w, &h, &x, &y)) continue;
        if (!first_name[0]) { strcpy(first_name, name); fw = w; fh = h; fx = x; fy = y; }
        if (strstr(line, " primary ")) {
            copy_string(app->monitor, sizeof app->monitor, name);
            app->capture_w = w; app->capture_h = h; app->capture_x = x; app->capture_y = y;
            free(out); return;
        }
    }
    if (!first_name[0]) die("could not determine primary monitor with xrandr");
    copy_string(app->monitor, sizeof app->monitor, first_name);
    app->capture_w = fw; app->capture_h = fh; app->capture_x = fx; app->capture_y = fy;
    free(out);
}

static void validate_metadata(App *app) {
    if (!app->title || !*app->title) die("missing stream title; pass -t/--title or set YOUTUBE_STREAM_TITLE");
    if (!app->description || !*app->description) die("missing stream description; pass -d/--description or set YOUTUBE_STREAM_DESCRIPTION");
    if (!app->thumbnail || !*app->thumbnail) die("missing stream thumbnail; pass -p/--thumbnail or set YOUTUBE_STREAM_THUMBNAIL");
    if (access(app->thumbnail, R_OK) != 0) die("thumbnail file does not exist: %s", app->thumbnail);
    char *real = realpath(app->thumbnail, NULL);
    if (real) { free(app->thumbnail); app->thumbnail = real; }
    const char *ext = strrchr(app->thumbnail, '.');
    if (!ext || (strcasecmp(ext, ".png") && strcasecmp(ext, ".jpg") && strcasecmp(ext, ".jpeg"))) die("thumbnail must be PNG/JPEG: %s", app->thumbnail);
}

static void write_state(App *app, const char *name, const char *value) {
    char *path = path_join(app->state_dir, name);
    FILE *f = fopen(path, "w");
    if (f) { fprintf(f, "%s\n", value ? value : ""); fclose(f); }
    free(path);
}

static void build_gsr_command(App *app) {
    argv_init(&app->cmd);
    argv_push(&app->cmd, "gpu-screen-recorder");
    argv_push(&app->cmd, "-w"); argv_push(&app->cmd, app->monitor);
    argv_push(&app->cmd, "-f"); argv_pushf(&app->cmd, "%d", app->fps);
    argv_push(&app->cmd, "-s"); argv_pushf(&app->cmd, "%dx%d", app->capture_w, app->capture_h);
    argv_push(&app->cmd, "-c"); argv_push(&app->cmd, "flv");
    argv_push(&app->cmd, "-k"); argv_push(&app->cmd, app->video_codec);
    argv_push(&app->cmd, "-bm"); argv_push(&app->cmd, "cbr");
    argv_push(&app->cmd, "-q"); argv_pushf(&app->cmd, "%d", app->video_bitrate);
    argv_push(&app->cmd, "-fm"); argv_push(&app->cmd, "cfr");
    argv_push(&app->cmd, "-tune"); argv_push(&app->cmd, app->gsr_tune);
    argv_push(&app->cmd, "-cursor"); argv_push(&app->cmd, "yes");
    argv_push(&app->cmd, "-keyint"); argv_pushf(&app->cmd, "%d", app->keyint);
    argv_push(&app->cmd, "-encoder"); argv_push(&app->cmd, app->encoder);
    argv_push(&app->cmd, "-fallback-cpu-encoding"); argv_push(&app->cmd, app->fallback_cpu);
    if (app->start_audio) {
        char *ab = strip_k_suffix(app->audio_bitrate);
        argv_push(&app->cmd, "-a"); argv_push(&app->cmd, app->gsr_audio_source);
        argv_push(&app->cmd, "-ac"); argv_push(&app->cmd, "aac");
        argv_push(&app->cmd, "-ab"); argv_push(&app->cmd, ab);
        free(ab);
    }
    argv_push(&app->cmd, "-o"); argv_push(&app->cmd, app->stream_url);
}

static void resolve_stream_url_fallback(App *app, Config *cfg) {
    if (app->stream_url && *app->stream_url) return;
    const char *url = config_get(cfg, "YOUTUBE_STREAM_URL");
    if (url && *url) { app->stream_url = xstrdup(url); return; }
    const char *key = config_get(cfg, "YOUTUBE_STREAM_KEY");
    const char *base = config_get_default(cfg, "YOUTUBE_STREAM_RTMP_BASE", "rtmp://a.rtmp.youtube.com/live2");
    if (!key || !*key) die("no stream URL/key from API or config");
    app->stream_url = xasprintf("%s/%s", base, key);
}

static void enable_gpu_performance(App *app, Config *cfg) {
    const char *mode = config_get_default(cfg, "YOUTUBE_STREAM_GPU_PERF", DEFAULT_GPU_PERF);
    if (!mode || !*mode || falseish(mode) || !strcasecmp(mode, "auto")) return;
    DIR *d = opendir("/sys/class/drm");
    if (!d) return;
    struct dirent *de;
    while ((de = readdir(d))) {
        if (strncmp(de->d_name, "card", 4)) continue;
        char *path = xasprintf("/sys/class/drm/%s/device/power_dpm_force_performance_level", de->d_name);
        FILE *f = fopen(path, "r");
        if (!f) { free(path); continue; }
        char old[64] = "";
        fgets(old, sizeof old, f);
        fclose(f);
        old[strcspn(old, "\n")] = 0;
        f = fopen(path, "w");
        if (f) {
            fprintf(f, "%s\n", mode);
            fclose(f);
            app->gpu_perf_path = path;
            app->gpu_perf_old = xstrdup(old);
            app->gpu_perf_set = 1;
            logx("GPU performance level: %s -> %s", old, mode);
            break;
        }
        free(path);
    }
    closedir(d);
}

static void restore_gpu_performance(App *app) {
    if (!app->gpu_perf_set || !app->gpu_perf_path || !app->gpu_perf_old) return;
    FILE *f = fopen(app->gpu_perf_path, "w");
    if (f) { fprintf(f, "%s\n", app->gpu_perf_old); fclose(f); }
    app->gpu_perf_set = 0;
}

static void spawn_outline(App *app) {
    if (!app->start_outline || !app->outline_helper) return;
    pid_t pid = fork();
    if (pid == 0) {
        char geom[128];
        snprintf(geom, sizeof geom, "%dx%d%+d%+d", app->capture_w, app->capture_h, app->capture_x, app->capture_y);
        char ppid[32];
        snprintf(ppid, sizeof ppid, "%ld", (long)getppid());
        execl(app->outline_helper, app->outline_helper, "--geometry", geom, "--parent-pid", ppid, (char *)NULL);
        _exit(127);
    }
    if (pid > 0) app->outline_pid = pid;
}

static void cleanup_outline(App *app) {
    if (app->outline_pid > 0) {
        kill(app->outline_pid, SIGTERM);
        waitpid(app->outline_pid, NULL, 0);
        app->outline_pid = 0;
    }
}

static void complete_broadcast(App *app) {
    if (app->stream_completed || !app->broadcast_id || !*app->broadcast_id || !app->api_helper) return;
    app->stream_completed = 1;
    char *argv[] = {app->api_helper, "complete", "--broadcast-id", app->broadcast_id, NULL};
    int status = 0;
    char *out = run_capture(argv, &status);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) warnx("failed to mark YouTube broadcast complete: %s", out);
    else logx("YouTube broadcast complete");
    free(out);
}

static int percent_from_gain(double gain) { return (int)(gain * 100.0 + 0.5); }
static double gain_from_percent(int pct) { return (double)pct / 100.0; }
static int clamp_percent(int v) { return v < 0 ? 0 : (v > 1500 ? 1500 : v); }

static int source_output_ids_by_app(const char *app_name, int *ids, int max_ids) {
    FILE *p = popen("pactl list source-outputs 2>/dev/null", "r");
    if (!p) return 0;
    char line[1024];
    int current = -1, found = 0, count = 0;
    while (fgets(line, sizeof line, p)) {
        if (!strncmp(line, "Source Output #", 15)) {
            if (current >= 0 && found && count < max_ids) ids[count++] = current;
            current = atoi(line + 15);
            found = 0;
        } else if (strstr(line, "application.name = ") || strstr(line, "media.name = ")) {
            char *q = strchr(line, '"');
            if (q) {
                char *e = strrchr(q + 1, '"');
                if (e) *e = 0;
                if (!strcmp(q + 1, app_name)) found = 1;
            }
        }
    }
    if (current >= 0 && found && count < max_ids) ids[count++] = current;
    pclose(p);
    return count;
}

static bool apply_source_output_gain(const char *app_name, int percent) {
    int ids[16];
    int n = source_output_ids_by_app(app_name, ids, 16);
    for (int i = 0; i < n; ++i) {
        char pct[32], id[32];
        snprintf(pct, sizeof pct, "%d%%", percent);
        snprintf(id, sizeof id, "%d", ids[i]);
        pid_t pid = fork();
        if (pid == 0) { execlp("pactl", "pactl", "set-source-output-volume", id, pct, (char *)NULL); _exit(127); }
        if (pid > 0) waitpid(pid, NULL, 0);
    }
    return n > 0;
}

static void apply_stream_gains(App *app) {
    if (!app->start_audio) return;
    bool ok1 = apply_source_output_gain(app->gsr_system_app, app->system_gain_percent);
    bool ok2 = apply_source_output_gain(app->gsr_mic_app, app->mic_gain_percent);
    if (ok1 && ok2) snprintf(app->tui_message, sizeof app->tui_message, "applied system %.2fx / mic %.2fx", gain_from_percent(app->system_gain_percent), gain_from_percent(app->mic_gain_percent));
    else snprintf(app->tui_message, sizeof app->tui_message, "waiting for GSR audio source-outputs...");
}

static void update_stream_status(App *app) {
    if (!app->api_helper || !app->broadcast_id || !*app->broadcast_id) return;
    char *argv[] = {app->api_helper, "status", "--broadcast-id", app->broadcast_id, NULL};
    int status = 0;
    char *out = run_capture(argv, &status);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) { free(out); return; }
    json_object *obj = json_tokener_parse(out);
    if (!obj) { free(out); return; }
    const char *life = jstr(obj, "life_cycle_status");
    const char *viewers = jstr(obj, "concurrent_viewers");
    if (*life) snprintf(app->tui_status, sizeof app->tui_status, "%s", life);
    if (*viewers) snprintf(app->tui_viewers, sizeof app->tui_viewers, "%s", viewers);
    else snprintf(app->tui_viewers, sizeof app->tui_viewers, "?");
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    strftime(app->tui_status_updated, sizeof app->tui_status_updated, "%H:%M:%S", &tm);
    json_object_put(obj);
    free(out);
}

static char *latest_capture_fps(App *app) {
    if (!app->log_file) return xstrdup("");
    FILE *f = fopen(app->log_file, "r");
    if (!f) return xstrdup("");
    char line[1024], last[1024] = "";
    while (fgets(line, sizeof line, f)) if (strstr(line, "update fps:")) snprintf(last, sizeof last, "%s", line);
    fclose(f);
    last[strcspn(last, "\n")] = 0;
    return xstrdup(last);
}

static char *latest_log_line(App *app) {
    if (!app->log_file) return xstrdup("");
    FILE *f = fopen(app->log_file, "r");
    if (!f) return xstrdup("");
    char line[1024], last[1024] = "";
    while (fgets(line, sizeof line, f)) {
        if (strstr(line, "update fps:") || strstr(line, "damage fps:")) continue;
        snprintf(last, sizeof last, "%s", line);
    }
    fclose(f);
    last[strcspn(last, "\n")] = 0;
    return xstrdup(last);
}

static void terminal_enter(App *app) {
    if (tcgetattr(STDIN_FILENO, &app->old_termios) == 0) {
        app->termios_saved = 1;
        struct termios raw = app->old_termios;
        raw.c_lflag &= ~(ECHO | ICANON);
        raw.c_cc[VMIN] = 0;
        raw.c_cc[VTIME] = 0;
        tcsetattr(STDIN_FILENO, TCSANOW, &raw);
    }
    printf("\033[?1049h\033[?25l\033[2J\033[H");
    fflush(stdout);
}

static void terminal_leave(App *app) {
    if (!app->termios_saved) return;
    tcsetattr(STDIN_FILENO, TCSANOW, &app->old_termios);
    app->termios_saved = 0;
    printf(W_RESET "\033[?25h\033[?1049l" W_RESET);
    fflush(stdout);
}

static void gain_bar(int percent, int width) {
    int max = 1200;
    if (percent > max) percent = max;
    int filled = percent * width / max;
    if (filled < 0) filled = 0;
    if (filled > width) filled = width;
    printf(W_ACCENT);
    for (int i = 0; i < filled; ++i) fputs("█", stdout);
    printf(W_MUTED);
    for (int i = filled; i < width; ++i) fputs("█", stdout);
    printf(W_RESET W_APP_BG);
}

static void draw_gain_row(const char *label, int percent, bool selected) {
    printf("%s%s %s%-11s%s %6d%%  %5.2fx  ", selected ? W_SELECTION_BG : "", selected ? "▶" : " ", W_COMMAND, label, W_RESET W_APP_BG, percent, gain_from_percent(percent));
    gain_bar(percent, 28);
    printf("%s\n", W_RESET W_APP_BG);
}

static void tui_draw(App *app) {
    char *capture = latest_capture_fps(app);
    char *logline = latest_log_line(app);
    const char *running = (app->stream_pid > 0 && kill(app->stream_pid, 0) == 0) ? "RUNNING" : "STOPPED";
    printf(W_APP_BG "\033[H\033[2J");
    printf(W_TOPBAR_BG W_BOLD " youtube-stream manager " W_RESET W_TOPBAR_BG " whale " W_BOLD "%s pid=%ld" W_RESET "\n", running, (long)app->stream_pid);
    printf(W_APP_BG "\n");
    printf(W_ACCENT W_BOLD "  Stream" W_RESET W_APP_BG "\n");
    printf("  %-14s " W_TEXT "%s" W_RESET W_APP_BG "\n", "title:", app->title);
    printf("  %-14s " W_COMMAND "%s" W_RESET W_APP_BG "\n", "watch:", app->watch_url ? app->watch_url : "?");
    printf("  %-14s %s @ %dfps, %dkbps, %s\n", "backend:", app->backend, app->fps, app->video_bitrate, app->privacy ? app->privacy : "unchanged");
    printf("  %-14s %s  viewers: %s  updated: %s\n", "YouTube:", app->tui_status, app->tui_viewers, app->tui_status_updated);
    printf("  %-14s %s\n\n", "capture:", *capture ? capture : "waiting for capture stats...");
    printf(W_ACCENT W_BOLD "  Live audio gain sent to stream" W_RESET W_APP_BG "\n");
    draw_gain_row("system", app->system_gain_percent, app->tui_selected == 0);
    draw_gain_row("mic", app->mic_gain_percent, app->tui_selected == 1);
    printf("\n" W_ACCENT W_BOLD "  Keys" W_RESET W_APP_BG "\n");
    printf("  " W_NORMAL "j/k" W_RESET W_APP_BG " select   " W_NORMAL "h/l" W_RESET W_APP_BG " ±5%%   " W_NORMAL "H/L" W_RESET W_APP_BG " ±25%%   " W_NORMAL "0" W_RESET W_APP_BG " mute   " W_NORMAL "=" W_RESET W_APP_BG " 100%%   " W_NORMAL "r" W_RESET W_APP_BG " refresh   " W_NORMAL "q" W_RESET W_APP_BG " stop\n\n");
    printf("  %-14s " W_SUCCESS "%s" W_RESET W_APP_BG "\n", "message:", *app->tui_message ? app->tui_message : "ready");
    printf("  %-14s %s\n", "encoder log:", *logline ? logline : (app->log_file ? app->log_file : ""));
    printf(W_RESET);
    fflush(stdout);
    free(capture); free(logline);
}

static int spawn_stream_to_log(App *app) {
    int fd = open(app->log_file, O_CREAT | O_TRUNC | O_WRONLY, 0600);
    if (fd < 0) die("failed to open %s: %s", app->log_file, strerror(errno));
    pid_t pid = fork();
    if (pid < 0) die("fork failed: %s", strerror(errno));
    if (pid == 0) {
        dup2(fd, STDERR_FILENO);
        dup2(fd, STDOUT_FILENO);
        close(fd);
        execvp(app->cmd.v[0], app->cmd.v);
        fprintf(stderr, "failed to exec %s: %s\n", app->cmd.v[0], strerror(errno));
        _exit(127);
    }
    close(fd);
    app->stream_pid = pid;
    return 0;
}

static void adjust_selected(App *app, int delta) {
    if (app->tui_selected == 0) app->system_gain_percent = clamp_percent(app->system_gain_percent + delta);
    else app->mic_gain_percent = clamp_percent(app->mic_gain_percent + delta);
    apply_stream_gains(app);
}

static void set_selected(App *app, int value) {
    if (app->tui_selected == 0) app->system_gain_percent = clamp_percent(value);
    else app->mic_gain_percent = clamp_percent(value);
    apply_stream_gains(app);
}

static int run_tui(App *app, Config *cfg) {
    (void)cfg;
    app->log_file = path_join(app->state_dir, "current-encoder.log");
    spawn_stream_to_log(app);
    sleep(1);
    apply_stream_gains(app);
    update_stream_status(app);
    int interval = atoi(nonnull(getenv("YOUTUBE_STREAM_TUI_STATUS_INTERVAL")));
    if (interval <= 0) interval = 15;
    time_t next_status = time(NULL) + interval;
    terminal_enter(app);
    int status = 0;
    while (!interrupted && app->stream_pid > 0 && kill(app->stream_pid, 0) == 0) {
        time_t now = time(NULL);
        if (now >= next_status) { update_stream_status(app); next_status = now + interval; }
        tui_draw(app);
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(STDIN_FILENO, &rfds);
        struct timeval tv = {.tv_sec = 0, .tv_usec = 250000};
        int rc = select(STDIN_FILENO + 1, &rfds, NULL, NULL, &tv);
        if (rc > 0 && FD_ISSET(STDIN_FILENO, &rfds)) {
            char ch = 0;
            if (read(STDIN_FILENO, &ch, 1) == 1) {
                switch (ch) {
                    case 'q': snprintf(app->tui_message, sizeof app->tui_message, "stopping stream..."); tui_draw(app); kill(app->stream_pid, SIGINT); break;
                    case 'j': if (app->tui_selected < 1) app->tui_selected++; break;
                    case 'k': if (app->tui_selected > 0) app->tui_selected--; break;
                    case 'h': adjust_selected(app, -5); break;
                    case 'l': adjust_selected(app, 5); break;
                    case 'H': adjust_selected(app, -25); break;
                    case 'L': adjust_selected(app, 25); break;
                    case '0': set_selected(app, 0); break;
                    case '=': set_selected(app, 100); break;
                    case 'r': apply_stream_gains(app); update_stream_status(app); break;
                }
            }
        }
        int wstatus;
        pid_t got = waitpid(app->stream_pid, &wstatus, WNOHANG);
        if (got == app->stream_pid) { status = wstatus; app->stream_pid = 0; break; }
    }
    if (app->stream_pid > 0) {
        kill(app->stream_pid, SIGINT);
        waitpid(app->stream_pid, &status, 0);
        app->stream_pid = 0;
    }
    terminal_leave(app);
    return status;
}

static void redact_and_write(const char *buf, size_t len, const char *stream_url) {
    char *text = strndup(buf, len);
    if (!text) return;
    char *redacted = redact_url(stream_url);
    char *p = text;
    while (*p) {
        char *hit = strstr(p, stream_url ? stream_url : "\001");
        if (!hit) { fputs(p, stderr); break; }
        fwrite(p, 1, (size_t)(hit - p), stderr);
        fputs(redacted, stderr);
        p = hit + strlen(stream_url);
    }
    free(redacted); free(text);
}

static int run_foreground(App *app) {
    int pipefd[2];
    if (pipe(pipefd) < 0) die("pipe failed: %s", strerror(errno));
    pid_t pid = fork();
    if (pid < 0) die("fork failed: %s", strerror(errno));
    if (pid == 0) {
        close(pipefd[0]);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[1]);
        execvp(app->cmd.v[0], app->cmd.v);
        fprintf(stderr, "failed to exec %s: %s\n", app->cmd.v[0], strerror(errno));
        _exit(127);
    }
    close(pipefd[1]);
    app->stream_pid = pid;
    char buf[4096];
    ssize_t n;
    while ((n = read(pipefd[0], buf, sizeof buf)) > 0) redact_and_write(buf, (size_t)n, app->stream_url);
    close(pipefd[0]);
    int status = 0;
    waitpid(pid, &status, 0);
    app->stream_pid = 0;
    return status;
}

static bool should_use_tui(App *app) {
    const char *term = getenv("TERM");
    return app->start_tui && !app->dry_run && isatty(STDIN_FILENO) && isatty(STDOUT_FILENO) && term && strcmp(term, "dumb");
}

static void cleanup(App *app) {
    if (app->stream_pid > 0) { kill(app->stream_pid, SIGINT); waitpid(app->stream_pid, NULL, 0); app->stream_pid = 0; }
    cleanup_outline(app);
    complete_broadcast(app);
    restore_gpu_performance(app);
}

static void usage(void) {
    puts("Usage: youtube-stream [options]\n"
         "  -t, --title TITLE       Stream title\n"
         "  -d, --description TEXT  Stream description\n"
         "  -p, --thumbnail PATH    Stream thumbnail\n"
         "  -v, --privacy STATUS    public, unlisted, or private\n"
         "  --setup                 Configure fallback stream key\n"
         "  --no-browser            Do not open watch page\n"
         "  --no-outline            Do not draw purple outline\n"
         "  --no-audio              Video only\n"
         "  --no-tui                Print logs instead of stream-manager TUI\n"
         "  --dry-run               Print encoder command only\n");
}

static void setup_key(App *app) {
    mkdir_p(app->config_dir, 0700);
    printf("Paste YouTube stream key: ");
    fflush(stdout);
    char key[512];
    if (!fgets(key, sizeof key, stdin)) die("failed to read key");
    key[strcspn(key, "\n")] = 0;
    FILE *f = fopen(app->config_file, "a");
    if (!f) die("failed to open %s: %s", app->config_file, strerror(errno));
    fprintf(f, "YOUTUBE_STREAM_KEY='%s'\n", key);
    fclose(f);
    chmod(app->config_file, 0600);
    logx("setup complete; config: %s", app->config_file);
}

int main(int argc, char **argv) {
    App app = {0};
    global_app = &app;
    app.start_browser = app.start_outline = app.start_audio = app.start_tui = 1;
    snprintf(app.tui_status, sizeof app.tui_status, "starting");
    snprintf(app.tui_viewers, sizeof app.tui_viewers, "?");
    snprintf(app.tui_status_updated, sizeof app.tui_status_updated, "never");
    char exe[PATH_MAX];
    ssize_t exe_len = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (exe_len > 0) { exe[exe_len] = 0; app.program_dir = dirname_dup(exe); }
    else app.program_dir = dirname_dup(argv[0]);
    const char *home = getenv("HOME");
    const char *xdg_config = getenv("XDG_CONFIG_HOME");
    const char *xdg_state = getenv("XDG_STATE_HOME");
    app.config_dir = xdg_config && *xdg_config ? xasprintf("%s/youtube-stream", xdg_config) : xasprintf("%s/.config/youtube-stream", home ? home : ".");
    app.config_file = path_join(app.config_dir, "config");
    app.state_dir = xdg_state && *xdg_state ? xasprintf("%s/youtube-stream", xdg_state) : xasprintf("%s/.local/state/youtube-stream", home ? home : ".");

    for (int i = 1; i < argc; ++i) {
        if ((!strcmp(argv[i], "-t") || !strcmp(argv[i], "--title")) && i + 1 < argc) app.title = xstrdup(argv[++i]);
        else if (!strncmp(argv[i], "--title=", 8)) app.title = xstrdup(argv[i] + 8);
        else if ((!strcmp(argv[i], "-d") || !strcmp(argv[i], "--description")) && i + 1 < argc) app.description = xstrdup(argv[++i]);
        else if (!strncmp(argv[i], "--description=", 14)) app.description = xstrdup(argv[i] + 14);
        else if ((!strcmp(argv[i], "-p") || !strcmp(argv[i], "--thumbnail")) && i + 1 < argc) app.thumbnail = xstrdup(argv[++i]);
        else if (!strncmp(argv[i], "--thumbnail=", 12)) app.thumbnail = xstrdup(argv[i] + 12);
        else if ((!strcmp(argv[i], "-v") || !strcmp(argv[i], "--privacy")) && i + 1 < argc) app.privacy = xstrdup(argv[++i]);
        else if (!strncmp(argv[i], "--privacy=", 10)) app.privacy = xstrdup(argv[i] + 10);
        else if (!strcmp(argv[i], "--setup")) app.setup_only = 1;
        else if (!strcmp(argv[i], "--no-browser")) app.start_browser = 0;
        else if (!strcmp(argv[i], "--no-outline")) app.start_outline = 0;
        else if (!strcmp(argv[i], "--no-audio")) app.start_audio = 0;
        else if (!strcmp(argv[i], "--no-tui")) app.start_tui = 0;
        else if (!strcmp(argv[i], "--dry-run")) app.dry_run = 1;
        else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) { usage(); return 0; }
        else if (argv[i][0] == '-') die("unknown option: %s", argv[i]);
        else if (!app.title) app.title = xstrdup(argv[i]);
        else die("unexpected extra argument: %s", argv[i]);
    }

    Config cfg = {0};
    load_config(&cfg, app.config_file);
    if (!app.title) app.title = xstrdup(config_get_default(&cfg, "YOUTUBE_STREAM_TITLE", ""));
    if (!app.description) app.description = xstrdup(config_get_default(&cfg, "YOUTUBE_STREAM_DESCRIPTION", ""));
    if (!app.thumbnail) app.thumbnail = xstrdup(config_get_default(&cfg, "YOUTUBE_STREAM_THUMBNAIL", ""));
    if (!app.privacy) app.privacy = xstrdup(config_get_default(&cfg, "YOUTUBE_STREAM_PRIVACY", ""));
    if (falseish(config_get(&cfg, "YOUTUBE_STREAM_TUI"))) app.start_tui = 0;

    if (app.setup_only) { setup_key(&app); return 0; }

    require_command("gpu-screen-recorder");
    require_command("xrandr");
    if (app.start_tui || app.start_audio) require_command("pactl");
    app.api_helper = find_api_helper(&app, &cfg);
    if (!app.api_helper) die("youtube-stream API helper not found; run make install");
    app.outline_helper = find_outline_helper(&app);
    if (app.start_outline && !app.outline_helper) warnx("outline helper not found; run make install");

    validate_metadata(&app);

    app.backend = xstrdup(config_get_default(&cfg, "YOUTUBE_STREAM_CAPTURE_BACKEND", DEFAULT_CAPTURE_BACKEND));
    if (!streq(app.backend, "gsr-direct-rtmp") && !streq(app.backend, "gsr-direct") && !streq(app.backend, "gpu-screen-recorder-direct")) {
        die("C rewrite currently supports the working direct GPU backend only: gsr-direct-rtmp");
    }
    app.fps = config_get_int(&cfg, "YOUTUBE_STREAM_FPS", DEFAULT_FPS);
    app.video_bitrate = config_get_int(&cfg, "YOUTUBE_STREAM_VIDEO_BITRATE_KBPS", DEFAULT_VIDEO_BITRATE_KBPS);
    app.video_codec = xstrdup(config_get_default(&cfg, "YOUTUBE_STREAM_VIDEO_CODEC", "h264"));
    app.keyint = config_get_int(&cfg, "YOUTUBE_STREAM_KEYINT", 2);
    app.encoder = xstrdup(config_get_default(&cfg, "YOUTUBE_STREAM_ENCODER", "gpu"));
    app.fallback_cpu = xstrdup(config_get_default(&cfg, "YOUTUBE_STREAM_FALLBACK_CPU_ENCODING", "no"));
    app.gsr_tune = xstrdup(config_get_default(&cfg, "YOUTUBE_STREAM_GSR_TUNE", DEFAULT_GSR_TUNE));
    app.audio_bitrate = xstrdup(config_get_default(&cfg, "YOUTUBE_STREAM_AUDIO_BITRATE", DEFAULT_AUDIO_BITRATE));
    app.mic_source_raw = xstrdup(config_get_default(&cfg, "YOUTUBE_STREAM_MIC_SOURCE", DEFAULT_MIC_SOURCE));
    app.system_source_raw = xstrdup(config_get_default(&cfg, "YOUTUBE_STREAM_SYSTEM_SOURCE", DEFAULT_SYSTEM_SOURCE));
    char *gsr_system = to_gsr_audio_source(app.system_source_raw);
    char *gsr_mic = to_gsr_audio_source(app.mic_source_raw);
    app.gsr_audio_source = xasprintf("%s|%s", gsr_system, gsr_mic);
    app.gsr_system_app = xasprintf("gsr-%s", gsr_system);
    app.gsr_mic_app = xasprintf("gsr-%s", gsr_mic);
    free(gsr_system); free(gsr_mic);
    app.mic_gain = config_get_double(&cfg, "YOUTUBE_STREAM_MIC_GAIN", DEFAULT_MIC_GAIN);
    app.system_gain = config_get_double(&cfg, "YOUTUBE_STREAM_SYSTEM_GAIN", DEFAULT_SYSTEM_GAIN);
    app.mic_gain_percent = percent_from_gain(app.mic_gain);
    app.system_gain_percent = percent_from_gain(app.system_gain);

    mkdir_p(app.state_dir, 0700);
    set_env_defaults(&app);
    if (!app.dry_run) apply_youtube_metadata_api(&app);
    resolve_stream_url_fallback(&app, &cfg);
    app.redacted_url = redact_url(app.stream_url);
    get_primary_monitor(&app);
    build_gsr_command(&app);

    if (app.dry_run) {
        printf("title: %s\ndescription: %s\nthumbnail: %s\nprivacy: %s\nbackend: %s\nmonitor: %s\ngeometry: %dx%d%+d%+d\nstream: %s\n",
               app.title, app.description, app.thumbnail, *app.privacy ? app.privacy : "unchanged", app.backend,
               app.monitor, app.capture_w, app.capture_h, app.capture_x, app.capture_y, app.redacted_url);
        print_command("GSR", &app.cmd, app.stream_url);
        return 0;
    }

    write_state(&app, "current-title", app.title);
    write_state(&app, "current-description", app.description);
    write_state(&app, "current-thumbnail", app.thumbnail);
    if (*app.privacy) write_state(&app, "current-privacy", app.privacy);
    if (app.watch_url) write_state(&app, "current-watch-url", app.watch_url);
    if (app.broadcast_id) write_state(&app, "current-broadcast-id", app.broadcast_id);
    char *monstate = xasprintf("%s %dx%d%+d%+d", app.monitor, app.capture_w, app.capture_h, app.capture_x, app.capture_y);
    write_state(&app, "current-monitor", monstate);
    free(monstate);

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    enable_gpu_performance(&app, &cfg);
    spawn_outline(&app);
    logx("title: %s", app.title);
    logx("description: %s", app.description);
    logx("thumbnail: %s", app.thumbnail);
    if (*app.privacy) logx("privacy: %s", app.privacy);
    logx("capturing primary monitor: %s (%dx%d%+d%+d), backend=%s, %dfps", app.monitor, app.capture_w, app.capture_h, app.capture_x, app.capture_y, app.backend, app.fps);
    if (app.start_audio) logx("audio: gpu-screen-recorder source=%s aac=%s", app.gsr_audio_source, app.audio_bitrate);
    else logx("audio: disabled");
    logx("streaming to: %s", app.redacted_url);
    logx(should_use_tui(&app) ? "opening stream manager TUI; press q or Ctrl+C to stop" : "press Ctrl+C to stop");

    int status = should_use_tui(&app) ? run_tui(&app, &cfg) : run_foreground(&app);
    terminal_leave(&app);
    cleanup(&app);
    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) logx("stream stopped");
    else if (interrupted) logx("stream stopped");
    else warnx("stream exited with status %d", status);
    argv_free(&app.cmd);
    return 0;
}
