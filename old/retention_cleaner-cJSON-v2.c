// Build:
//   sudo apt install libcjson-dev
//   gcc -D_XOPEN_SOURCE=700 -O2 -Wall -Wextra retention_cleaner.c -o retention_cleaner -lcjson

#define _XOPEN_SOURCE 700
#include <ftw.h>
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <cjson/cJSON.h>

/*
  Assumptions:
   - config.json format:
     {
       "retention": {
         "default": 30,
         "1001": 60,
         "1017": 120
       }
     }

   - directory layout to evaluate:
     /.../<DEVICE>/<YYYY>/<MM>/<DD>/<HH>/<mm>
     last 5 tokens are YYYY MM DD HH mm
     device is token at index n-6
*/

typedef struct {
    char root[PATH_MAX];
    int max_open_fd;
    bool dry_run;
    int default_days;
    cJSON *retention_obj; // owns reference (we keep pointer and free on exit)
} CleanerConfig;

static CleanerConfig G_CFG;

static void die(const char *msg) {
    perror(msg);
    exit(EXIT_FAILURE);
}

static bool read_file_into_buf(const char *path, char **out_buf) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return false; }
    long sz = ftell(f);
    if (sz < 0) { fclose(f); return false; }
    rewind(f);
    char *buf = malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return false; }
    size_t n = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[n] = '\0';
    *out_buf = buf;
    return true;
}

static void load_config_cjson(const char *config_path) {
    memset(&G_CFG, 0, sizeof(G_CFG));
    snprintf(G_CFG.root, sizeof(G_CFG.root), "."); // default root
    G_CFG.max_open_fd = 32;
    G_CFG.dry_run = true;
    G_CFG.default_days = 30;
    G_CFG.retention_obj = NULL;

    char *buf = NULL;
    if (!read_file_into_buf(config_path, &buf)) {
        fprintf(stderr, "Failed to read config file: %s\n", config_path);
        exit(EXIT_FAILURE);
    }

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) {
        fprintf(stderr, "cJSON parse error (config)\n");
        exit(EXIT_FAILURE);
    }

    cJSON *ret = cJSON_GetObjectItemCaseSensitive(root, "retention");
    if (!ret || !cJSON_IsObject(ret)) {
        cJSON_Delete(root);
        fprintf(stderr, "config.json must contain an object 'retention'\n");
        exit(EXIT_FAILURE);
    }

    cJSON *def = cJSON_GetObjectItemCaseSensitive(ret, "default");
    if (!def || !cJSON_IsNumber(def)) {
        cJSON_Delete(root);
        fprintf(stderr, "retention.default (number) is required\n");
        exit(EXIT_FAILURE);
    }

    G_CFG.default_days = (int)def->valuedouble;

    // Keep reference to retention object: increase its refcount by duplicating pointer in new object
    // cJSON doesn't have refcount; easiest is to keep 'ret' pointer and keep whole root alive.
    // We'll store root in retention_obj and free it later.
    G_CFG.retention_obj = cJSON_Duplicate(ret, 1); // deep copy; we'll free this copy on exit
    cJSON_Delete(root);
    // now G_CFG.retention_obj is a standalone object containing mapping
}

typedef struct {
    int year, month, day, hour, minute;
} PathTime;

static bool tokenize_path_to_array(const char *path, char **tokens, int *count) {
    static char tmp[PATH_MAX];
    if (strlen(path) >= sizeof(tmp)) return false;
    strcpy(tmp, path);
    int n = 0;
    char *save = NULL;
    for (char *p = strtok_r(tmp, "/", &save); p && n < 256; p = strtok_r(NULL, "/", &save)) {
        tokens[n++] = p;
    }
    *count = n;
    return (n > 0);
}

static bool parse_tail_time_from_tokens(char **tokens, int n, PathTime *pt) {
    if (n < 5) return false;
    const char *sY = tokens[n-5];
    const char *sM = tokens[n-4];
    const char *sD = tokens[n-3];
    const char *sH = tokens[n-2];
    const char *sm = tokens[n-1];
    char *endp = NULL;
    long Y = strtol(sY, &endp, 10); if (*sY == '\0' || *endp != '\0') return false;
    long M = strtol(sM, &endp, 10); if (*sM == '\0' || *endp != '\0') return false;
    long D = strtol(sD, &endp, 10); if (*sD == '\0' || *endp != '\0') return false;
    long H = strtol(sH, &endp, 10); if (*sH == '\0' || *endp != '\0') return false;
    long m = strtol(sm, &endp, 10); if (*sm == '\0' || *endp != '\0') return false;
    if (Y < 1970 || M < 1 || M > 12 || D < 1 || D > 31 || H < 0 || H > 23 || m < 0 || m > 59) return false;
    pt->year = (int)Y; pt->month = (int)M; pt->day = (int)D; pt->hour = (int)H; pt->minute = (int)m;
    return true;
}

static const char *device_from_tokens(char **tokens, int n) {
    if (n < 6) return NULL;
    return tokens[n-6];
}

static time_t pathtime_to_epoch(const PathTime *pt) {
    struct tm tmv;
    memset(&tmv, 0, sizeof(tmv));
    tmv.tm_year = pt->year - 1900;
    tmv.tm_mon  = pt->month - 1;
    tmv.tm_mday = pt->day;
    tmv.tm_hour = pt->hour;
    tmv.tm_min  = pt->minute;
    tmv.tm_sec  = 0;
    // mktime uses local TZ. If you want UTC, implement timegm or adjust TZ.
    return mktime(&tmv);
}

static int retention_days_for_device(const char *device) {
    if (!device || !G_CFG.retention_obj) return G_CFG.default_days;
    cJSON *val = cJSON_GetObjectItemCaseSensitive(G_CFG.retention_obj, device);
    if (val && cJSON_IsNumber(val)) return (int)val->valuedouble;
    return G_CFG.default_days;
}

static bool is_older_than_days_bypath(const char *path, int days) {
    char *tokens[256];
    int n = 0;
    if (!tokenize_path_to_array(path, tokens, &n)) return false;
    PathTime pt;
    if (!parse_tail_time_from_tokens(tokens, n, &pt)) return false;
    time_t ptime = pathtime_to_epoch(&pt);
    if (ptime == (time_t)-1) return false;
    time_t now = time(NULL);
    double diff_days = difftime(now, ptime) / (60.0*60.0*24.0);
    return diff_days >= (double)days;
}

static int remove_path_entry(const char *path) {
    // Because nftw called with FTW_DEPTH, directory should be empty when visited
    // Use remove() which handles files and empty directories
    return remove(path);
}

static int process_path_cb(const char *path, const struct stat *sb, int typeflag, struct FTW *ftwbuf) {
    (void)sb; (void)ftwbuf;
    if (typeflag != FTW_D && typeflag != FTW_DP) return 0;

    // decide device and retention days
    char *tokens[256];
    int n = 0;
    if (!tokenize_path_to_array(path, tokens, &n)) return 0;
    const char *device = device_from_tokens(tokens, n);
    int days = retention_days_for_device(device);

    if (is_older_than_days_bypath(path, days)) {
        if (G_CFG.dry_run) {
            printf("[DRY-RUN] device=%s days=%d delete: %s\n", device?device:"(none)", days, path);
        } else {
            if (remove_path_entry(path) == 0) {
                printf("[DELETED] device=%s days=%d %s\n", device?device:"(none)", days, path);
            } else {
                fprintf(stderr, "[ERROR] delete failed: %s (errno=%d)\n", path, errno);
            }
        }
    }
    return 0;
}

static void usage(const char *pname) {
    fprintf(stderr,
        "Usage: %s -c config.json -r ROOT [--force] [--fd N]\n"
        "  -c/--config  config.json path (required)\n"
        "  -r/--root    root directory to scan (required)\n"
        "  --force      perform deletion (default: dry-run)\n"
        "  --fd N       nftw max open fds (default 32)\n", pname);
}

int main(int argc, char **argv) {
    const char *config_path = NULL;
    const char *root = NULL;
    bool force = false;
    int fd_override = -1;

    for (int i = 1; i < argc; ++i) {
        if ((strcmp(argv[i], "-c") == 0 || strcmp(argv[i], "--config") == 0) && i + 1 < argc) {
            config_path = argv[++i];
        } else if ((strcmp(argv[i], "-r") == 0 || strcmp(argv[i], "--root") == 0) && i + 1 < argc) {
            root = argv[++i];
        } else if (strcmp(argv[i], "--force") == 0) {
            force = true;
        } else if (strcmp(argv[i], "--fd") == 0 && i + 1 < argc) {
            fd_override = atoi(argv[++i]);
        } else {
            usage(argv[0]);
            return EXIT_FAILURE;
        }
    }

    if (!config_path || !root) { usage(argv[0]); return EXIT_FAILURE; }

    load_config_cjson(config_path);
    strncpy(G_CFG.root, root, sizeof(G_CFG.root)-1);
    if (fd_override > 0) G_CFG.max_open_fd = fd_override;
    if (force) G_CFG.dry_run = false;

    printf("Root: %s\nDRY-RUN: %s\nnftw FD: %d\nDefault retention: %d days\n",
           G_CFG.root, G_CFG.dry_run ? "true" : "false", G_CFG.max_open_fd, G_CFG.default_days);

    int flags = FTW_PHYS | FTW_DEPTH;
    if (nftw(G_CFG.root, process_path_cb, G_CFG.max_open_fd, flags) != 0) {
        die("nftw");
    }

    if (G_CFG.retention_obj) cJSON_Delete(G_CFG.retention_obj);
    return 0;
}

