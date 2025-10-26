#define _XOPEN_SOURCE 700
#include <ftw.h>
#include <dirent.h>
#include <errno.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <libgen.h>   // dirname
#include <limits.h>
#include <unistd.h>

// ---- 글로벌 옵션들 ----
static const char *g_root_path = "/data";   // 옵션/설정에서 세팅
static bool gDry_run = false;
static long gRetention_secs = 30L * 24 * 3600; // 예: 30일 (config.json에서 로드해 대체)

// ---- Heap 엔트리 ----
typedef struct {
    time_t expire;
    char  *path;   // 삭제할 "디렉터리"의 절대경로 (분 단위 디렉터리)
} HeapEntry;

typedef struct {
    HeapEntry *a;
    size_t size, cap;
} MinHeap;

static void heap_init(MinHeap *h) { h->a=NULL; h->size=0; h->cap=0; }

static int entry_less(const HeapEntry *x, const HeapEntry *y) {
    if (x->expire != y->expire) return x->expire < y->expire;
    return strcmp(x->path, y->path) < 0;
}

static void heap_swap(HeapEntry *x, HeapEntry *y) {
    HeapEntry t=*x; *x=*y; *y=t;
}

static void heap_reserve(MinHeap *h, size_t need) {
    if (h->cap >= need) return;
    size_t ncap = h->cap ? h->cap*2 : 256;
    if (ncap < need) ncap = need;
    h->a = (HeapEntry*)realloc(h->a, ncap*sizeof(HeapEntry));
    h->cap = ncap;
}

static void heap_push(MinHeap *h, HeapEntry e) {
    heap_reserve(h, h->size+1);
    size_t i = h->size++;
    h->a[i] = e;
    // up-heap
    while (i>0) {
        size_t p = (i-1)/2;
        if (!entry_less(&h->a[i], &h->a[p])) break;
        heap_swap(&h->a[i], &h->a[p]); i = p;
    }
}

static bool heap_peek(MinHeap *h, HeapEntry *out) {
    if (h->size==0) return false;
    if (out) *out = h->a[0];
    return true;
}

static bool heap_pop(MinHeap *h, HeapEntry *out) {
    if (h->size==0) return false;
    if (out) *out = h->a[0];
    h->a[0] = h->a[--h->size];
    // down-heap
    size_t i=0;
    for (;;) {
        size_t l=2*i+1, r=2*i+2, s=i;
        if (l<h->size && entry_less(&h->a[l], &h->a[s])) s=l;
        if (r<h->size && entry_less(&h->a[r], &h->a[s])) s=r;
        if (s==i) break;
        heap_swap(&h->a[i], &h->a[s]); i=s;
    }
    return true;
}

static void heap_free(MinHeap *h) {
    for (size_t i=0;i<h->size;i++) free(h->a[i].path);
    free(h->a);
    h->a=NULL; h->size=h->cap=0;
}

// ---- 경로에서 시간 파싱: /root/company/device/YYYY/MM/DD/HH/mm ----
// 성공 시 true, out_epoch에 time_t 저장 (로컬타임 가정; UTC 쓰려면 timegm)
static bool parse_epoch_from_path(const char *path, time_t *out_epoch) {
    // 끝에서 5개 디렉토리(YYYY/MM/DD/HH/mm)를 토큰으로 뽑음
    // 안전한 복사본
    char buf[PATH_MAX]; strncpy(buf, path, sizeof(buf)); buf[sizeof(buf)-1]=0;
    // 토큰 수를 세기 위해 슬래시 기준 분해
    // 간단화를 위해 strtok_r로 전체 토큰을 배열에 모은다
    char *save=NULL, *tok=NULL;
    char *parts[64]; int n=0;
    for (tok=strtok_r(buf, "/", &save); tok && n<64; tok=strtok_r(NULL, "/", &save)) parts[n++]=tok;
    if (n < 5) return false;

    int mm  = atoi(parts[n-1]);
    int HH  = atoi(parts[n-2]);
    int DD  = atoi(parts[n-3]);
    int MON = atoi(parts[n-4]); // 1..12
    int YYYY= atoi(parts[n-5]);

    struct tm tmv = {0};
    tmv.tm_year = YYYY - 1900;
    tmv.tm_mon  = MON - 1;
    tmv.tm_mday = DD;
    tmv.tm_hour = HH;
    tmv.tm_min  = mm;
    tmv.tm_sec  = 0;

    time_t t = mktime(&tmv); // 로컬타임 기준. UTC를 쓰려면 timegm(&tmv)
    if (t == (time_t)-1) return false;
    *out_epoch = t;
    return true;
}

// ---- nftw 콜백에서 "분 디렉토리"만 힙에 등록 ----
// level = .../YYYY/MM/DD/HH/mm  까지 내려왔을 때 FTW_D (pre) 또는 FTW_DP (post) 시점 중 선택.
// 여기선 FTW_D(선주문)에서 등록만 하고, 실제 삭제는 워커 루프가 처리.
static MinHeap g_heap;

static int cb_register_minute_dir(const char *fpath, const struct stat *sb, int typeflag, struct FTW *ftwbuf) {
    (void)sb;
    if (typeflag != FTW_D) return 0;   // 디렉토리만 고려
    // 트리 깊이가 7(= data/company/device/YYYY/MM/DD/HH/mm)라는 가정이면 여기에서 필터링 가능
    if (ftwbuf->level < 7) return 0;

    time_t create_epoch;
    if (!parse_epoch_from_path(fpath, &create_epoch)) return 0;

    time_t expire = create_epoch + gRetention_secs;

    // 엔트리 등록
    HeapEntry e = { .expire = expire, .path = strdup(fpath) };
    heap_push(&g_heap, e);
    return 0;
}

// ---- 삭제 워커 (힙 top 만기까지 반복) ----
static void process_due_deletes(void) {
    time_t now = time(NULL);
    HeapEntry e;
    while (heap_peek(&g_heap, &e) && e.expire <= now) {
        heap_pop(&g_heap, &e); // 꺼낸다
        if (gDry_run) {
            printf("[DRY-RUN] Would delete: %s (expire=%ld)\n", e.path, (long)e.expire);
        } else {
            if (remove(e.path) == 0) {
                // 디렉토리가 비어있을 때만 성공; 비어있지 않으면 ENOTEMPTY.
                printf("Deleted: %s\n", e.path);
            } else if (errno == ENOTEMPTY) {
                // 아직 하위 파일이 남아있음 → 다음 주기에 재시도(혹은 하위부터 파일 먼저 지우기)
                // 간단히: 재등록(약간 뒤로 미룸)
                e.expire = now + 60;  // 1분 후 재시도 (전략은 상황에 맞춰 조정)
                heap_push(&g_heap, e);
                continue; // free(e.path) 금지: 재사용
            } else {
                perror(e.path);
            }
        }
        free(e.path);
    }
}

// ---- 초기 스캔 + 주기 처리의 예시 main 루프 ----
int main(int argc, char **argv) {
    // TODO: getopt_long로 g_root_path, gRetention_secs, gDry_run 등 세팅
    //       config.json(cJSON) 로딩 → 회사/디바이스별 retention이면 map으로 보관 후 콜백에서 조회

    heap_init(&g_heap);

    // 1) 초기 스캔: 모든 "분" 디렉터리를 힙에 등록
    if (nftw(g_root_path, cb_register_minute_dir, 32, FTW_PHYS | FTW_ACTIONRETVAL) != 0) {
        perror("nftw");
        // 계속 진행할지 종료할지 결정
    }

    // 2) 메인 워커 루프 (데몬이라면 sleep 간격 조절)
    for (;;) {
        process_due_deletes();
        // 새 디렉터리 반영: inotify가 없다면 주기적으로 신규 스캔 추가
        // 예: 5분마다 가벼운 재스캔을 하여 미등록 항목을 보강
        sleep(5);
    }

    heap_free(&g_heap);
    return 0;
}

