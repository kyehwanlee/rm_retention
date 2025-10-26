#!/usr/bin/env bash
# seed_test_data.sh
# Create test directory tree under /data for retention_cleaner testing.
# - Structure: /data/<company>/<device>/<YYYY>/<MM>/<DD>/<HH>/<mm>/
# - Each minute-dir contains ~50 dummy files.
# - Includes weekly samples from the last 6 months up to BASE_DATE.
# - Ensures company IDs 1001 and 1017 exist.
# - Also creates folders older than 30/60/120/150 days relative to BASE_DATE.

set -euo pipefail

# ==== Config ====
BASE_DATE="2025-10-24"        # 기준 "현재" 날짜 (요청 조건에 맞춤). 바꾸고 싶으면 수정.
ROOT="/data"                  # 생성 루트
FILES_PER_MINUTE=50           # 더미 파일 개수
FILE_SIZE_KB=1                # 각 파일 크기(KB) — dd로 1KB씩 생성
MONTHS_BACK=6                 # 지난 6개월
WEEK_STEP_DAYS=7              # 주 1회 샘플

# 회사/디바이스 샘플. 1001, 1017 반드시 포함.
COMPANIES=(1001 1017 1289 2466)
DEVICES=(2001 2002 2003 2466 3001)

# ==== Helpers ====

rand_between() { # inclusive
  local min="$1" max="$2"
  echo $(( RANDOM % (max - min + 1) + min ))
}

pad2() {
  printf "%02d" "$1"
}

# $1: company  $2: device  $3: YYYY  $4: MM  $5: DD  $6: HH  $7: mm
make_entry() {
  local c="$1" d="$2" Y="$3" M="$4" D="$5" H="$6" m="$7"
  local dir="${ROOT}/${c}/${d}/${Y}/${M}/${D}/${H}/${m}"
  mkdir -p -- "${dir}"

  # 더미 파일 50개 생성 (1KB씩). 너무 느리면 FILES_PER_MINUTE 줄이세요.
  for i in $(seq 1 "${FILES_PER_MINUTE}"); do
    # 1KB 랜덤 데이터 파일(정말 랜덤 필요 없으면 'touch'로 대체 가능)
    dd if=/dev/urandom of="${dir}/file_${i}.bin" bs=1K count="${FILE_SIZE_KB}" status=none
  done
}

# $1: 날짜(YYYY-MM-DD), 회사/디바이스는 순환/랜덤
weekly_sample_for_date() {
  local date_str="$1"
  local Y M D H m c d

  Y="$(date -d "${date_str}" +%Y)"
  M="$(date -d "${date_str}" +%m)"
  D="$(date -d "${date_str}" +%d)"
  H="$(pad2 "$(rand_between 0 23)")"
  m="$(pad2 "$(rand_between 0 59)")"

  # 회사/디바이스 선택(라운드로빈 + 랜덤 섞기)
  local idx=$(( RANDOM % ${#COMPANIES[@]} ))
  c="${COMPANIES[$idx]}"
  d="${DEVICES[$(( RANDOM % ${#DEVICES[@]} ))]}"

  make_entry "$c" "$d" "$Y" "$M" "$D" "$H" "$m"
}

# threshold 일수 샘플을 특정 회사ID로 생성
# $1: company_id, $2: days_ago
threshold_sample_for_company() {
  local c="$1" days="$2"
  local date_str Y M D H m d
  date_str="$(date -d "${BASE_DATE} - ${days} days" +%F)"
  Y="$(date -d "${date_str}" +%Y)"
  M="$(date -d "${date_str}" +%m)"
  D="$(date -d "${date_str}" +%d)"
  H="$(pad2 "$(rand_between 0 23)")"
  m="$(pad2 "$(rand_between 0 59)")"
  d="${DEVICES[$(( RANDOM % ${#DEVICES[@]} ))]}"
  make_entry "$c" "$d" "$Y" "$M" "$D" "$H" "$m"
}

# ==== Main ====
WEEK_SECS=$(( 7 * 24 * 60 * 60 ))

# 1) 최근 6개월 구간: BASE_DATE부터 6개월 전까지, 1주일 간격으로 샘플 생성
echo "[*] Creating weekly samples from ${BASE_DATE} back ${MONTHS_BACK} months..."
start_ts="$(date -d "${BASE_DATE}" +%s)"
end_ts="$(date -d "${BASE_DATE} - ${MONTHS_BACK} months" +%s)"

# 앞으로 시간이 흐르지 않도록 큰->작은으로 내려가며 생성
cur_ts="${start_ts}"
while (( cur_ts >= end_ts )); do
  the_date="$(date -d "@${cur_ts}" +%F)"
  weekly_sample_for_date "${the_date}"
  # 1주 전으로 이동
  #cur_ts="$(date -d "@${cur_ts} - ${WEEK_STEP_DAYS} days" +%s)"
  cur_ts=$(( cur_ts - WEEK_SECS ))
done

# 2) 보존 임계(30/60/120/150일) 샘플: 1001, 1017 두 회사에 대해 명시적으로 생성
echo "[*] Creating explicit threshold samples (30/60/120/150 days ago) for 1001 and 1017..."
for days in 30 60 120 150; do
  threshold_sample_for_company "1001" "${days}"
  threshold_sample_for_company "1017" "${days}"
done

# 3) 샘플 완료 안내
echo "[*] Done. Example paths:"
echo "    ${ROOT}/1001/<device>/<YYYY>/<MM>/<DD>/<HH>/<mm>/file_1.bin"
echo "    ${ROOT}/1017/<device>/<YYYY>/<MM>/<DD>/<HH>/<mm>/file_1.bin"
echo
echo "[*] Tip: To preview some created directories:"
echo "    find ${ROOT} -type d -path '${ROOT}/*/*/*/*/*/*/*' | head -5"

