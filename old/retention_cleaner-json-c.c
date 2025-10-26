// Build:
//   gcc -D_XOPEN_SOURCE=700 -O2 -Wall -Wextra retention_cleaner.c -o retention_cleaner \
//       $(pkg-config --cflags --libs json-c)

#define _XOPEN_SOURCE 700
#include <ftw.h>
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <json-c/json.h>

// ---------------------------
// Config
// ---------------------------
typedef struct {
    char   root[PATH_MAX];        // 탐색 루트 (CLI로 받음, 기본 ".")
    int    max_open_fd;           // nftw 동시 FD
    bool   dry_run;               // --force 없으면 DRY-RUN
    struct json_object *ret_map;  // {"default":int, "<device>":int, ...}
    int    default_days;          // ret_map["default"]
} CleanerConfig;

static CleanerConfig G_CFG;

static void die(const char *msg) {
    perror(msg);
    exit(EXIT_FAILURE);
}

static bool read_file(const char *path, char **out) {
    FILE *fp = fopen(path, "rb");
    if (!fp) return false;
    if (fseek(fp, 0, SEEK_END) != 0) { fclose(fp); return false; }
    long sz = ftell(fp); if (sz < 0) { fclose(fp); return false; }
    rewind(fp);
    char *buf = (char*)malloc((size_t)sz + 1);
    if (!buf) { fclose(fp); return false; }
    size_t n = fread(buf, 1, (size_t)sz, fp);
    fclose(fp);
    buf[n] = '\0';
    *out = buf;
    return true;
}

static void load_config(const char *config_path) {
    memset(&G_CFG, 0, sizeof(G_CFG));
    snprintf(G_CFG.root, sizeof(G_CFG.root), "."); // 기본값
    G_CFG.max_open_fd = 32;
    G_CFG.dry_run = true;

    char *json_text = NULL;
    if (!read_file(config_path, &json_text)) {
        die("read config");
    }

    struct json_tokener *tok = json_tokener_new();
    struct json_object *root = json_tokener_parse_ex(tok, json_text, (int)strlen(json_text));
    enum json_tokener_error jerr = json_tokener_get_error(tok);
    json_tokener_free(tok);
    free(json_text);

    if (!root || jerr != json_tokener_success) die("json parse");

    // root["retention"] 객체 필수
    struct json_object *ret;
    if (!json_object_object_get_ex(root, "retention", &ret) ||
        !json_object_is_type(ret, json_type_object)) {
        json_object_put(root);
        fprintf(stderr, "config.json: \"retention\" object is required\n");
        exit(EXIT_FAILURE);
    }

    // default 필수
    struct json_object *dv;
    if (!json_object_object_get_ex(ret, "default", &dv) ||
        !json_object_is_type(dv, json_type_int)) {
        json_object_put(root);
        fprintf(stderr, "config.json: retention.default (int) is required\n");
        exit(EXIT_FAILURE);
    }

    G_CFG.ret_map = ret;                 // 소유권은 root에 있었음 → 참조 유지 위해 incref
    json_object_get(G_CFG.ret_map);
    G_CFG.default_days = json_object_get_int(dv);

    json_object_put(root);               // root 해제( ret_map은 incref 했으므로 살아있음 )
}

// ---------------------------
// 경로 파싱: /.../<device>/<YYYY>/<MM>/<DD>/<HH>/<mm>
// 끝에서 5개는 시간, 그 앞이 device 라고 가정
// ---------------------------
typedef struct {
    int year, month, day, hour, minute;
} PathTime;

static bool tokenize_path(const char *path, char **out_tokens, int *out_n) {
    static char buf[PATH_MAX];
    snprintf(buf, sizeof(buf), "%s", path);
    int n = 0;
    char *save = NULL;
    for (char *p = strtok_r(buf, "/", &save); p && n < 256; p = strtok_r(NULL, "/", &save)) {
        out_tokens[n++] = p;
    }
    *out_n = n;
    return (n > 0);
}

static bool parse_tail_time(char **tokens, int n, PathTime *pt) {
    if (n < 5) return false;
    const char *tY = tokens[n-5], *tM = tokens[n-4], *tD = tokens[n-3], *tH = tokens[n-2], *tm = tokens[n-1];
    char *e = NULL;
    long Y = strtol(tY, &e, 10); if (*tY=='\0'||*e!='\0') return false;
    long M = strtol(tM, &e, 10); if (*tM=='\0'||*e!='\0') return false;
    long D = strtol(tD, &e, 10); if (*tD=='\0'||*e!='\0') return false;
    long H = strtol(tH, &e, 10); if (*tH=='\0'||*e!='\0') return false;
    long m = strtol(tm, &e, 10); if (*tm=='\0'||*e!='\0') return false;
    if (Y<1970 || M<1||M>12 || D<1||D>31 || H<0||H>23 || m<0||m>59) return false;
    pt->year=Y; pt->month=M; pt->day=D; pt->hour=H; pt->minute=m;
    return true;
}

static const char* extract_device(char **tokens, int n) {
    // 끝의 5개가 시간이면 device는 n-6
    if (n < 6) return NULL;
    return tokens[n-6];
}

static time_t path_time_to_epoch(const PathTime *pt) {
    struct tm t = {0};
    t.tm_year = pt->year - 1900;
    t.tm_mon  = pt->month - 1;
    t.tm_mday = pt->day;
    t.tm_hour = pt->hour;
    t.tm_min  = pt->minute;
    t.tm_sec  = 0;
    return mktime(&t); // 로컬 TZ 기준. 필요시 timegm 대체 구현 가능.
}

// ---------------------------
// retention 조회
// ---------------------------
static int retention_days_for_device(const char *device) {
    if (!device || !*device) return G_CFG.default_days;
    struct json_object *v = NULL;
    if (json_object_object_get_ex(G_CFG.ret_map, device, &v) &&
        json_object_is_type(v, json_type_int)) {
        return json_object_get_int(v);
    }
    return G_CFG.default_days;
}

static bool is_older_than_days(const char *path, int days) {
    char *toks[256]; int n=0;
    if (!tokenize_path(path, toks, &n)) return false;

    PathTime pt;
    if (!parse_tail_time(toks, n, &pt)) return false;

    time_t ptime = path_time_to_epoch(&pt);
    if (ptime == (time_t)-1) return false;

    time_t now = time(NULL);
    double diff_days = difftime(now, ptime) / (60.0*60.0*24.0);
    return diff_days >= (double)days;
}

// ---------------------------
// 삭제 & nftw 콜백
// ---------------------------
static int remove_tree(const char *path) {
    // FTW_DEPTH 로 내려오므로 파일 이후 디렉토리에서 rmdir가 가능.
    //

