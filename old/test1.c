/*
 * retention_cleaner.c
 *  - Parse config.json and remove old directories per device rule.
 *  - CLI options: -c/--config, -r/--root, --force, --fd N
 *
 *  Author: <Your Name>
 */

#define _GNU_SOURCE
#define _XOPEN_SOURCE 700

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <limits.h>
#include <ftw.h>
#include <time.h>
#include <errno.h>
#include <getopt.h>
#include <cjson/cJSON.h>

/*----------------------------------------------------------
 *  enum / typedef
 *----------------------------------------------------------*/
typedef enum {
    OPT_FORCE = 1000,     /* long-option codes (ASCII와 충돌 피함) */
    OPT_FD
} optcode_t;

typedef struct option longopt_t;

/*----------------------------------------------------------
 *  global options
 *----------------------------------------------------------*/
static const char *config_path = NULL;
static const char *root_path   = NULL;
static bool  force_delete = false;
static int   fd_limit     = 32;

/*----------------------------------------------------------
 *  helpers
 *----------------------------------------------------------*/
static int parse_int(const char *s, int *out)
{
    char *end = NULL;
    long v = strtol(s, &end, 10);
    if (!s || *s == '\0' || *end != '\0')
        return -1;
    if (v < 0 || v > INT_MAX)
        return -1;
    *out = (int)v;
    return 0;
}

/*----------------------------------------------------------
 *  JSON parsing (retention rules)
 *----------------------------------------------------------*/
typedef struct {
    int default_days;
    cJSON *rules;   /* {"1001":60, "1017":120, ...} */
} retention_cfg_t;

static bool load_config(const char *path, retention_cfg_t *cfg)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) { perror(path); return false; }

    fseek(fp, 0, SEEK_END);
    long len = ftell(fp);
    rewind(fp);

    char *buf = malloc((size_t)len + 1);
    fread(buf, 1, (size_t)len, fp);
    buf[len] = '\0';
    fclose(fp);

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) { fprintf(stderr,"JSON parse error\n"); return false; }

    cJSON *ret = cJSON_GetObjectItem(root, "retention");
    if (!ret || !cJSON_IsObject(ret)) { cJSON_Delete(root); return false; }

    cJSON *def = cJSON_GetObjectItem(ret, "default");
    cfg->default_days = def && cJSON_IsNumber(def) ? def->valueint : 30;

    cfg->rules = cJSON_Duplicate(ret, 1);
    cJSON_Delete(root);
    return true;
}

static int retention_for_device(const retention_cfg_t *cfg, const char *device)
{
    if (!cfg || !cfg->rules) return 30;
    cJSON *v = cJSON_GetObjectItem(cfg->rules, device);
    return (v && cJSON_IsNumber(v)) ? v->valueint : cfg->default_days;
}

/*----------------------------------------------------------
 *  path-time parsing : .../<device>/YYYY/MM/DD/HH/mm
 *----------------------------------------------------------*/
typedef struct { int y,m,d,H,M; } ptime_t;

static bool parse_path_time(const char *path, ptime_t *pt, char **device_out)
{
    static char buf[PATH_MAX];
    strncpy(buf, path, sizeof(buf));
    buf[PATH_MAX-1] = 0;

    char *tok[256]; int n=0; char *save=NULL;
    for (char *p=strtok_r(buf,"/",&save); p && n<256; p=strtok_r(NULL,"/",&save))
        tok[n++]=p;
    if (n<6) return false;

    *device_out = tok[n-6];
    pt->y=atoi(tok[n-5]); pt->m=atoi(tok[n-4]);
    pt->d=atoi(tok[n-3]); pt->H=atoi(tok[n-2]); pt->M=atoi(tok[n-1]);
    return true;
}

static bool older_than(const ptime_t *pt, int days)
{
    struct tm t={0};
    t.tm_year=pt->y-1900; t.tm_mon=pt->m-1;
    t.tm_mday=pt->d; t.tm_hour=pt->H; t.tm_min=pt->M;
    time_t when=mktime(&t);
    if (when==(time_t)-1) return false;

    time_t now=time(NULL);
    double diff=difftime(now,when)/(60*60*24);
    return diff>=days;
}

/*----------------------------------------------------------
 *  nftw callback
 *----------------------------------------------------------*/
static retention_cfg_t CFG;

static int process_entry(const char *path, const struct stat *sb,
                         int typeflag, struct FTW *ftwbuf)
{
    (void)sb; (void)ftwbuf;
    if (typeflag!=FTW_D && typeflag!=FTW_DP) return 0;

    ptime_t pt; char *device=NULL;
    if (!parse_path_time(path,&pt,&device)) return 0;

    int keep_days=retention_for_device(&CFG,device);
    if (older_than(&pt,keep_days)) {
        if (force_delete) {
            if (remove(path)==0)
                printf("[DELETED] %s\n",path);
            else
                perror(path);
        } else {
            printf("[DRY-RUN] %s (>%d days)\n",path,keep_days);
        }
    }
    return 0;
}

/*----------------------------------------------------------
 *  main : argument parsing
 *----------------------------------------------------------*/
int main(int argc, char **argv)
{
    static longopt_t longopts[] = {
        {"config", required_argument, NULL, 'c'},
        {"root",   required_argument, NULL, 'r'},
        {"force",  no_argument,       NULL, OPT_FORCE},
        {"fd",     required_argument, NULL, OPT_FD},
        {NULL,0,NULL,0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "c:r:", longopts, NULL)) != -1) {
        switch (opt) {
        case 'c': config_path = optarg; break;
        case 'r': root_path   = optarg; break;
        case OPT_FORCE: force_delete = true; break;
        case OPT_FD:
            if (parse_int(optarg,&fd_limit)!=0) {
                fprintf(stderr,"Invalid --fd value\n"); return EXIT_FAILURE;
            }
            break;
        default:
            fprintf(stderr,"Usage: %s -c <config> -r <root> [--force] [--fd N]\n",argv[0]);
            return EXIT_FAILURE;
        }
    }

    if (!config_path || !root_path) {
        fprintf(stderr,"Both --config and --root are required.\n");
        return EXIT_FAILURE;
    }

    printf("Config: %s\nRoot: %s\nForce: %s\nFD limit: %d\n",
           config_path, root_path, force_delete?"true":"false", fd_limit);

    if (!load_config(config_path,&CFG))
        return EXIT_FAILURE;

    int flags = FTW_PHYS | FTW_DEPTH;
    if (nftw(root_path, process_entry, fd_limit, flags) != 0)
        perror("nftw");

    if (CFG.rules) cJSON_Delete(CFG.rules);
    return 0;
}

