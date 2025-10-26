// retention_cleaner-cJSON-v3.c
// 고정 경로 스키마(토큰 8개):
// [0]=data [1]=company [2]=device [3]=YYYY [4]=MM [5]=DD [6]=HH [7]=mm
// minute 디렉터리 내부 항목은 토큰 9개 이상.
// JSON 예: { "retention": { "default": 30, "1001": 60 } }

#define _DEFAULT_SOURCE
#define _XOPEN_SOURCE 700

#include <ftw.h>
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <getopt.h>
#include <ctype.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cjson/cJSON.h>

/* -------- 설정 -------- */
typedef struct {
    int   default_days;
    cJSON *retention_obj;   // "retention" 객체 deep copy
} RETN_CONFIG;

static RETN_CONFIG g_cfg;
static const char *g_root = NULL;   // 반드시 "/data"
static bool g_dry_run = false;

/* -------- 유틸 -------- */
typedef struct { int year, month, day, hour, minute, second; } ptime_t;

static inline int is_digits(const char *s) {
    if (!s || !*s) return 0;
    for (const unsigned char *p=(const unsigned char*)s; *p; ++p)
        if (!isdigit(*p)) return 0;
    return 1;
}

static time_t ptime_to_epoch(const ptime_t *pt) {
    struct tm t = {0};
    t.tm_year = pt->year - 1900;
    t.tm_mon  = pt->month - 1;
    t.tm_mday = pt->day;
    t.tm_hour = pt->hour;
    t.tm_min  = pt->minute;
    t.tm_sec  = pt->second;
    t.tm_isdst = -1;
    return mktime(&t);   // 로컬 타임존
}

static int retention_days_for_device(const char *device) {
    if (g_cfg.retention_obj) {
        cJSON *d = cJSON_GetObjectItem(g_cfg.retention_obj, device);
        if (cJSON_IsNumber(d)) return d->valueint;
        cJSON *def = cJSON_GetObjectItem(g_cfg.retention_obj, "default");
        if (cJSON_IsNumber(def)) return def->valueint;
    }
    return (g_cfg.default_days > 0) ? g_cfg.default_days : 30;
}

/* -------- JSON 로드 -------- */
static bool load_json_config(const char *path, RETN_CONFIG *out) {
    FILE *fp = fopen(path, "r");
    if (!fp) { perror("fopen"); return false; }

    if (fseek(fp, 0, SEEK_END) != 0) { perror("fseek"); fclose(fp); return false; }
    long sz = ftell(fp);
    if (sz < 0) { perror("ftell"); fclose(fp); return false; }
    rewind(fp);

    char *buf = (char*)malloc((size_t)sz + 1);
    if (!buf) { perror("malloc"); fclose(fp); return false; }

    size_t nread = fread(buf, 1, (size_t)sz, fp);
    fclose(fp);
    buf[nread] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (!root) {
        const char *err = cJSON_GetErrorPtr();
        if (err) fprintf(stderr, "JSON parse error before: %s\n", err);
        free(buf);
        return false;
    }

    cJSON *retention = cJSON_GetObjectItem(root, "retention");
    if (!cJSON_IsObject(retention)) {
        fprintf(stderr, "JSON: 'retention' object missing\n");
        cJSON_Delete(root); free(buf); return false;
    }

    cJSON *def = cJSON_GetObjectItem(retention, "default");
    out->default_days  = cJSON_IsNumber(def) ? def->valueint : 30;
    out->retention_obj = cJSON_Duplicate(retention, 1);

    cJSON_Delete(root);
    free(buf);
    return true;
}

/* -------- 경로 파싱 (토큰 8개 고정) -------- */
typedef struct {
    char  buf[PATH_MAX];
    char *tok[256];
    int   ntok;
} tokbuf_t;

/*
 * fpath 전체를 '/'로 나눠 토큰화한다.
 * 기대: tok[0]="data"
 *       tok[1]=company, tok[2]=device, tok[3]=Y, tok[4]=M, tok[5]=D, tok[6]=H, tok[7]=mm
 * 반환: 1=minute 스키마 일치, 0=불일치
 * out: company, device, pt, ntok(전체 토큰 수)
 */
static int parse_schema_tokens8(const char *fpath,
                                tokbuf_t *tb,
                                const char **company,
                                const char **device,
                                ptime_t *pt,
                                int *ntok_out)
{
    if (snprintf(tb->buf, sizeof(tb->buf), "%s", fpath) >= (int)sizeof(tb->buf))
        return 0;

    tb->ntok = 0;
    char *saveptr = NULL;
    for (char *t = strtok_r(tb->buf, "/", &saveptr);
         t && tb->ntok < (int)(sizeof(tb->tok)/sizeof(tb->tok[0]));
         t = strtok_r(NULL, "/", &saveptr))
    {
        tb->tok[tb->ntok++] = t;
    }

    if (tb->ntok < 8) return 0;
    if (strcmp(tb->tok[0], "data") != 0) return 0;  // 첫 토큰은 반드시 data

    // 고정 인덱스
    const char *comp = tb->tok[1];
    const char *dev  = tb->tok[2];
    const char *sY   = tb->tok[3];
    const char *sM   = tb->tok[4];
    const char *sD   = tb->tok[5];
    const char *sH   = tb->tok[6];
    const char *sMin = tb->tok[7];

    if (!is_digits(sY) || !is_digits(sM) || !is_digits(sD) ||
        !is_digits(sH) || !is_digits(sMin))
        return 0;

    int Y  = atoi(sY), M = atoi(sM), D = atoi(sD);
    int Hr = atoi(sH), Mi = atoi(sMin);
    if (Y < 1970 || M<1 || M>12 || D<1 || D>31 || Hr<0 || Hr>23 || Mi<0 || Mi>59)
        return 0;

    *company = comp;
    *device  = dev;

    pt->year   = Y;
    pt->month  = M;
    pt->day    = D;
    pt->hour   = Hr;
    pt->minute = Mi;
    pt->second = 0;

    *ntok_out  = tb->ntok;
    return 1;
}

/* -------- nftw 콜백 -------- */
static int cb_process(const char *fpath, const struct stat *sb, int typeflag, struct FTW *ftwbuf)
{
    (void)sb; (void)ftwbuf;

    tokbuf_t tb;
    const char *company = NULL, *device = NULL;
    ptime_t pt;
    int ntok = 0;

    if (!parse_schema_tokens8(fpath, &tb, &company, &device, &pt, &ntok))
        return 0; // minute 스키마가 아니면 스킵

    // 보존기간 만료 판단
    int keep_days = retention_days_for_device(device);
    time_t ts = ptime_to_epoch(&pt);
    double age_days = difftime(time(NULL), ts) / 86400.0;
    if (!(age_days > (double)keep_days)) return 0; // 보존기간 이내면 아무 것도 안 함

    // 판정: minute 디렉터리 자체 / minute 내부
    // - minute 디렉터리 자체: 토큰이 정확히 8개 (data..mm)
    // - minute 내부 항목: 토큰이 9개 이상 (data..mm/<something>)
    bool is_minute_dir = (ntok == 8);
    bool under_minute  = (ntok >= 9);

    switch (typeflag) {
    case FTW_F:
    case FTW_SL:
    case FTW_SLN:
        if (!under_minute) return 0;  // minute 내부 파일/링크만 삭제
        if (g_dry_run) {
            printf("[DRY FILE] %s (dev=%s keep=%d age=%.1f)\n",
                   fpath, device, keep_days, age_days);
        } else {
            if (remove(fpath) == 0) printf("[DEL FILE] %s\n", fpath);
            else perror(fpath);
        }
        break;

    case FTW_DP: // 자식 처리 후 디렉터리
        if (!(is_minute_dir || under_minute)) return 0; // data, company 등 상위는 건드리지 않음
        if (g_dry_run) {
            printf("[DRY DIR ] %s (dev=%s keep=%d age=%.1f)\n",
                   fpath, device, keep_days, age_days);
        } else {
            if (remove(fpath) == 0) printf("[DEL DIR ] %s\n", fpath);
            else perror(fpath);
        }
        break;

    default:
        break; // FTW_D 등 무시
    }

    return 0;
}

/* -------- 메인/옵션 -------- */
static void usage(const char *argv0) {
    fprintf(stderr,
      "Usage: %s -c config.json -r /data [--dry-run] [--fd N]\n"
      "  -c, --config  JSON path (required)\n"
      "  -r, --root    MUST be /data (required)\n"
      "  --dry-run     print actions only\n"
      "  --fd N        nftw max open fds (default 32)\n",
      argv0);
}

int main(int argc, char **argv)
{
    const char *config_path = NULL;
    int maxfds = 32;

    enum { O_DRYRUN = 1, O_FD };
    static struct option longopts[] = {
        { "config",  required_argument, NULL, 'c' },
        { "root",    required_argument, NULL, 'r' },
        { "dry-run", no_argument,       NULL, O_DRYRUN },
        { "fd",      required_argument, NULL, O_FD },
        { NULL, 0, NULL, 0 }
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "c:r:", longopts, NULL)) != -1) {
        switch (opt) {
        case 'c': config_path = optarg; break;
        case 'r': g_root      = optarg; break;
        case O_DRYRUN: g_dry_run = true; break;
        case O_FD: maxfds = atoi(optarg); break;
        default: usage(argv[0]); return EXIT_FAILURE;
        }
    }
    if (!config_path || !g_root) { usage(argv[0]); return EXIT_FAILURE; }
    if (strcmp(g_root, "/data") != 0) {
        fprintf(stderr, "Error: root must be '/data' for fixed 8-token schema.\n");
        return EXIT_FAILURE;
    }

    if (!load_json_config(config_path, &g_cfg)) {
        fprintf(stderr, "Failed to load config.\n");
        return EXIT_FAILURE;
    }

    printf("Root=%s  Mode=%s  default=%d days\n",
           g_root, g_dry_run ? "DRY-RUN" : "DELETE", g_cfg.default_days);

    int flags = FTW_PHYS | FTW_DEPTH; // 파일/하위부터 방문
    if (nftw(g_root, cb_process, maxfds, flags) == -1) {
        perror("nftw");
        if (g_cfg.retention_obj) cJSON_Delete(g_cfg.retention_obj);
        return EXIT_FAILURE;
    }

    if (g_cfg.retention_obj) cJSON_Delete(g_cfg.retention_obj);
    return EXIT_SUCCESS;
}

