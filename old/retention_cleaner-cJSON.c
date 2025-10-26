/* Feature test macros must come BEFORE any #include */
#define _XOPEN_SOURCE 700   /* expose nftw(), struct FTW, FTW_* */
#define _GNU_SOURCE          /* expose timegm(); _DEFAULT_SOURCE도 가능 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <ftw.h>        /* nftw, struct FTW, FTW_* flags */
#include <sys/stat.h>
#include <unistd.h>     /* unlink, rmdir */
#include <time.h>
#include <ctype.h>

#include <cjson/cJSON.h>  /* cJSON package header: /usr/include/cjson/cJSON.h */

#define MAX_PATH 4096


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
    int default_days;
    struct {
        char company_id[32];
        int days;
    } custom[128];
    int custom_count;
} Config;

typedef struct {
    char root[MAX_PATH];
    Config cfg;
    int execute;          /* 0: dry-run, 1: delete */
    time_t now;           /* current UTC epoch */
    unsigned long scanned_dirs;
    unsigned long minute_dirs_checked;
    unsigned long deleted;
} Context;

static Context ctx;

/* ---------------------- Utilities ---------------------- */

static int load_config(const char *path, Config *cfg) {
    FILE *f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "open config.json failed: %s\n", strerror(errno));
        return -1;
    }
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return -1; }
    long sz = ftell(f);
    if (sz < 0) { fclose(f); return -1; }
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return -1; }

    char *buf = (char*)malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return -1; }
    size_t nread = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[nread] = '\0';

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) {
        fprintf(stderr, "invalid JSON in config\n");
        return -1;
    }

    cJSON *retention = cJSON_GetObjectItem(root, "retention");
    if (!cJSON_IsObject(retention)) {
        fprintf(stderr, "config missing 'retention' object\n");
        cJSON_Delete(root);
        return -1;
    }

    cfg->default_days = 30;
    cfg->custom_count = 0;

    /* iterate keys in retention */
    cJSON *it = retention->child;
    while (it) {
        if (strcmp(it->string, "default") == 0) {
            if (cJSON_IsNumber(it)) cfg->default_days = it->valuedouble;
        } else {
            if (cJSON_IsNumber(it)) {
                if (cfg->custom_count < (int)(sizeof(cfg->custom)/sizeof(cfg->custom[0]))) {
                    snprintf(cfg->custom[cfg->custom_count].company_id,
                             sizeof(cfg->custom[cfg->custom_count].company_id),
                             "%s", it->string);
                    cfg->custom[cfg->custom_count].days = (int)it->valuedouble;
                    cfg->custom_count++;
                }
            }
        }
        it = it->next;
    }

    cJSON_Delete(root);
    return 0;
}

static int get_retention_days(const Config *cfg, const char *company_id) {
    for (int i = 0; i < cfg->custom_count; i++) {
        if (strcmp(cfg->custom[i].company_id, company_id) == 0)
            return cfg->custom[i].days;
    }
    return cfg->default_days;
}

/* Parse UTC timestamp from strings; use timegm (GNU extension) */
static int parse_timestamp_utc(const char *year, const char *month, const char *day,
                               const char *hour, const char *minute, time_t *out) {
    struct tm t;
    memset(&t, 0, sizeof(t));
    t.tm_year = atoi(year) - 1900;
    t.tm_mon  = atoi(month) - 1;
    t.tm_mday = atoi(day);
    t.tm_hour = atoi(hour);
    t.tm_min  = atoi(minute);
    t.tm_sec  = 0;

    /* Validate ranges a bit */
    if (t.tm_year < 0 || t.tm_mon < 0 || t.tm_mon > 11 ||
        t.tm_mday < 1 || t.tm_mday > 31 ||
        t.tm_hour < 0 || t.tm_hour > 23 ||
        t.tm_min < 0 || t.tm_min > 59) {
        return -1;
    }

    time_t ts = timegm(&t);  /* UTC epoch; requires _GNU_SOURCE/_DEFAULT_SOURCE */
    if (ts == (time_t)-1) {
        /* timegm can return -1 legitimately for 1969-12-31 etc., but we treat as ok */
    }
    *out = ts;
    return 0;
}

/* Minute-dir check: path depth must be .../company/device/year/month/day/hour/minute */
static int is_minute_dir_old(const char *path) {
    char buf[MAX_PATH];
    strncpy(buf, path, sizeof(buf)-1);
    buf[sizeof(buf)-1] = '\0';

    char *tokens[64];
    int n = 0;
    for (char *p = strtok(buf, "/"); p && n < 64; p = strtok(NULL, "/")) {
        tokens[n++] = p;
    }
    /* Need at least 8 components: root/... company device year month day hour minute */
    if (n < 8) return 0;

    /* Take from the end to be robust: */
    const char *minute     = tokens[n - 1];
    const char *hour       = tokens[n - 2];
    const char *day        = tokens[n - 3];
    const char *month      = tokens[n - 4];
    const char *year       = tokens[n - 5];
    const char *device     = tokens[n - 6]; (void)device;
    const char *company_id = tokens[n - 7];

    time_t ts;
    if (parse_timestamp_utc(year, month, day, hour, minute, &ts) != 0)
        return 0;

    int days = get_retention_days(&ctx.cfg, company_id);
    time_t cutoff = ctx.now - (time_t)days * 24 * 3600;
    return (ts < cutoff);
}

/* -------------- Recursive delete (rm -rf using nftw) -------------- */

static int rm_tree_cb(const char *fpath, const struct stat *sb, int typeflag, struct FTW *ftwbuf) {
    (void)sb; (void)ftwbuf;
    int rv = 0;

    /* Depth-first (FTW_DEPTH) traversal guaranteed; delete files then dirs */
    if (typeflag == FTW_DP || typeflag == FTW_D) {
        rv = rmdir(fpath);
    } else {
        rv = unlink(fpath);
    }
    if (rv != 0) {
        fprintf(stderr, "remove '%s' failed: %s\n", fpath, strerror(errno));
    }
    return 0; /* continue even if some entries fail */
}

static int rm_tree(const char *path) {
    /* FTW_PHYS: no symlink traversal; FTW_DEPTH: post-order (delete children first) */
    return nftw(path, rm_tree_cb, 32, FTW_PHYS | FTW_DEPTH);
}

/* -------------- Main walk: select minute dirs and delete -------------- */

static int process_path(const char *path, const struct stat *sb, int typeflag, struct FTW *ftwbuf) {
    (void)sb;

    if (typeflag != FTW_D && typeflag != FTW_DP) {
        return 0; /* only consider directories */
    }

    ctx.scanned_dirs++;

    /* Expected levels:
       ... root(0)/company(1)/device(2)/year(3)/month(4)/day(5)/hour(6)/minute(7) */
    if (ftwbuf->level == 7) {
        ctx.minute_dirs_checked++;
        if (is_minute_dir_old(path)) {
            if (ctx.execute) {
                if (rm_tree(path) == 0) {
                    printf("[DEL] %s\n", path);
                } else {
                    fprintf(stderr, "[ERR] rm_tree failed for %s: %s\n", path, strerror(errno));
                }
            } else {
                printf("[DRY] would delete %s\n", path);
            }
            ctx.deleted++;
        }
    }

    if (ctx.minute_dirs_checked && (ctx.minute_dirs_checked % 1000 == 0)) {
        printf("progress: scanned_dirs=%lu minute_dirs=%lu deleted=%lu\n",
               ctx.scanned_dirs, ctx.minute_dirs_checked, ctx.deleted);
        fflush(stdout);
    }
    return 0;
}

/* ------------------------------ main ------------------------------ */

int main(int argc, char *argv[]) {
    if (argc < 4) {
        fprintf(stderr,
            "Usage: %s <root_path> <config.json> <dry-run|execute>\n"
            "Example:\n"
            "  %s /data config.json dry-run\n"
            "  %s /data config.json execute\n",
            argv[0], argv[0], argv[0]);
        return 1;
    }

    /* root path */
    strncpy(ctx.root, argv[1], sizeof(ctx.root)-1);
    ctx.root[sizeof(ctx.root)-1] = '\0';

    /* mode */
    ctx.execute = (strcmp(argv[3], "execute") == 0);

    /* time now in UTC */
    ctx.now = time(NULL);

    /* load config (cJSON) */
    if (load_config(argv[2], &ctx.cfg) != 0) {
        fprintf(stderr, "Failed to load config from %s\n", argv[2]);
        return 1;
    }

    printf("Starting cleaner root=%s mode=%s now(UTC)=%s",
           ctx.root, ctx.execute ? "EXECUTE" : "DRY-RUN", asctime(gmtime(&ctx.now)));

    /* Walk the tree; FTW_PHYS(no symlink), FTW_MOUNT는 필요시 고려 */
    if (nftw(ctx.root, process_path, 32, FTW_PHYS | FTW_DEPTH) != 0) {
        fprintf(stderr, "nftw failed on %s: %s\n", ctx.root, strerror(errno));
        /* still print summary */
    }

    printf("Done. scanned_dirs=%lu minute_dirs=%lu deleted=%lu\n",
           ctx.scanned_dirs, ctx.minute_dirs_checked, ctx.deleted);
    return 0;
}

