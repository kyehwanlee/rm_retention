/*
 * retention_cleaner.c
 * Lightweight file retention cleaner (C version)
 *
 * Compile:
 *   gcc -o retention_cleaner retention_cleaner.c -lcjson -lftw
 *
 * Usage:
 *   ./retention_cleaner /data config.json dry-run
 *   ./retention_cleaner /data config.json execute
 *
 * Author: (anonymous submission)
 */

#define _XOPEN_SOURCE 700
#define _DEFAULT_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ftw.h>
#include <sys/stat.h>
#include <unistd.h>
#include <time.h>
#include <cjson/cJSON.h>

#define MAX_PATH 4096

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
    int execute;
    time_t now;
    unsigned long scanned;
    unsigned long deleted;
} Context;

Context ctx;

/* ---------- Utility: trim ---------- */
static void trim(char *s) {
    char *p = s;
    while (*p == ' ' || *p == '\t' || *p == '\n') p++;
    if (p != s) memmove(s, p, strlen(p) + 1);
    int len = strlen(s);
    while (len > 0 && (s[len - 1] == ' ' || s[len - 1] == '\t' || s[len - 1] == '\n'))
        s[--len] = 0;
}

/* ---------- Parse config.json ---------- */
int load_config(const char *path, Config *cfg) {
    FILE *f = fopen(path, "r");
    if (!f) {
        perror("open config.json");
        return -1;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc(sz + 1);
    fread(buf, 1, sz, f);
    buf[sz] = 0;
    fclose(f);

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) {
        fprintf(stderr, "invalid JSON\n");
        return -1;
    }

    cJSON *retention = cJSON_GetObjectItem(root, "retention");
    if (!retention) {
        fprintf(stderr, "config missing 'retention'\n");
        cJSON_Delete(root);
        return -1;
    }

    cfg->custom_count = 0;
    cfg->default_days = 30; // fallback
    cJSON *it = retention->child;
    while (it) {
        if (strcmp(it->string, "default") == 0)
            cfg->default_days = it->valueint;
        else {
            strcpy(cfg->custom[cfg->custom_count].company_id, it->string);
            cfg->custom[cfg->custom_count].days = it->valueint;
            cfg->custom_count++;
        }
        it = it->next;
    }

    cJSON_Delete(root);
    return 0;
}

/* ---------- Get retention days ---------- */
int get_retention_days(Config *cfg, const char *company_id) {
    for (int i = 0; i < cfg->custom_count; i++) {
        if (strcmp(cfg->custom[i].company_id, company_id) == 0)
            return cfg->custom[i].days;
    }
    return cfg->default_days;
}

/* ---------- Parse timestamp from path ---------- */
int parse_timestamp(const char *year, const char *month, const char *day,
                    const char *hour, const char *minute, time_t *out) {
    struct tm t;
    memset(&t, 0, sizeof(t));
    t.tm_year = atoi(year) - 1900;
    t.tm_mon = atoi(month) - 1;
    t.tm_mday = atoi(day);
    t.tm_hour = atoi(hour);
    t.tm_min = atoi(minute);
    t.tm_sec = 0;
    *out = timegm(&t); // UTC
    return 0;
}

/* ---------- Check if a directory is old ---------- */
int is_old_dir(const char *path) {
    // Expect structure: /data/company/device/year/month/day/hour/min
    // Tokenize
    char buf[MAX_PATH];
    strcpy(buf, path);
    char *tokens[16];
    int n = 0;
    char *tok = strtok(buf, "/");
    while (tok && n < 16) {
        tokens[n++] = tok;
        tok = strtok(NULL, "/");
    }
    if (n < 8) return 0; // not deep enough

    const char *company_id = tokens[n - 7];
    const char *year = tokens[n - 4];
    const char *month = tokens[n - 3];
    const char *day = tokens[n - 2];
    const char *hour = tokens[n - 1];
    const char *minute = tokens[n];

    time_t ts;
    parse_timestamp(year, month, day, hour, minute, &ts);
    int days = get_retention_days(&ctx.cfg, company_id);

    time_t cutoff = ctx.now - (time_t)days * 24 * 3600;
    return ts < cutoff;
}

/* ---------- Called by nftw for each file/dir ---------- */
int process_path(const char *path, const struct stat *sb, int typeflag, struct FTW *ftwbuf) {
    if (typeflag != FTW_D) return 0; // only directories
    ctx.scanned++;

    // we only care when depth == 7 (minute directory)
    if (ftwbuf->level != 7) return 0;

    if (is_old_dir(path)) {
        if (ctx.execute) {
            if (remove(path) == 0) {
                printf("[DEL] %s\n", path);
            } else {
                perror("delete");
            }
        } else {
            printf("[DRY] would delete %s\n", path);
        }
        ctx.deleted++;
    }

    if (ctx.scanned % 1000 == 0) {
        printf("scanned=%lu deleted=%lu\n", ctx.scanned, ctx.deleted);
    }
    return 0;
}

/* ---------- main ---------- */
int main(int argc, char *argv[]) {
    if (argc < 4) {
        fprintf(stderr, "Usage: %s <root_path> <config.json> <dry-run|execute>\n", argv[0]);
        return 1;
    }

    strcpy(ctx.root, argv[1]);
    ctx.execute = (strcmp(argv[3], "execute") == 0);
    ctx.now = time(NULL);

    if (load_config(argv[2], &ctx.cfg) != 0) {
        fprintf(stderr, "Failed to load config\n");
        return 1;
    }

    printf("Starting cleaner root=%s mode=%s now=%s\n",
           ctx.root, ctx.execute ? "EXECUTE" : "DRY-RUN", asctime(gmtime(&ctx.now)));

    if (nftw(ctx.root, process_path, 32, FTW_PHYS | FTW_DEPTH) != 0) {
        perror("nftw");
    }

    printf("Done. scanned=%lu deleted=%lu\n", ctx.scanned, ctx.deleted);
    return 0;
}

