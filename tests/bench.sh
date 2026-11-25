#!/bin/bash
# bench.sh — 부하 테스트 표 만들기: 동시 처리 모드(pool/thread/fork) × 요청 종류별 초당 처리량
#
# 사용:  make bench && bash tests/bench.sh
#        N=50000 C=100 bash tests/bench.sh     (요청 수, 동시 연결 수 조절)
#        DOCROOT=/tmp/www bash tests/bench.sh  (정적 파일 루트 바꾸기)
# 결과:  표를 화면에 출력하고 bench_result.md 로도 저장
N=${N:-20000}          # 정적/프록시 요청 수
NC=${NC:-2000}         # CGI 요청 수 (프로세스를 매번 만들어서 느리므로 적게)
C=${C:-50}             # 동시 연결 수
DOCROOT=${DOCROOT:-www}
P=8091   # 시연 서버(8080)와 겹치지 않게
[ -x ./bench ] || { echo "먼저 make bench"; exit 1; }

# 동시 연결 1000개를 열려면 파일 디스크립터 한도가 넉넉해야 함 (macOS 기본 256)
#   여기서 띄우는 bench와 서버가 모두 이 한도를 물려받는다
ulimit -n 10240 2>/dev/null || ulimit -n "$(ulimit -Hn)" 2>/dev/null

# Reverse Proxy 측정용 백엔드 두 개 (시연 서버와 겹치지 않게 8011, 8012 + 임시 규칙 파일)
ROUTES=$(mktemp /tmp/cweb_routes.XXXXXX)
printf 'route /api/ 127.0.0.1:8011 127.0.0.1:8012\n' > "$ROUTES"
./server -p 8011 -r demo/backend1 > /dev/null 2>&1 & B1=$!
./server -p 8012 -r demo/backend2 > /dev/null 2>&1 & B2=$!
trap 'kill -INT $B1 $B2 2>/dev/null; rm -f "$ROUTES"' EXIT

rps() { ./bench "$@" | awk '/초당 요청 수/{print $5}'; }
p99() { ./bench "$@" | awk '{for(i=1;i<NF;i++) if($i=="p99") print $(i+1)}'; }

out="bench_result.md"
{
echo "# 부하 테스트 결과 ($(uname -s), 코어 $(getconf _NPROCESSORS_ONLN 2>/dev/null)개, 동시 연결 $C)"
echo
echo "| 모드 | 정적 keep-alive | 정적 새 연결 | CGI (env.sh) | Reverse Proxy |"
echo "|---|---|---|---|---|"
} > "$out"

for m in pool thread fork; do
  ./server -p $P -r "$DOCROOT" -c "$ROUTES" -m $m > /dev/null 2>&1 & S=$!
  sleep 0.7
  a=$(rps -n $N -c $C -k http://127.0.0.1:$P/index.html)
  b=$(rps -n $N -c $C    http://127.0.0.1:$P/index.html)
  c=$(rps -n $NC -c $C -k http://127.0.0.1:$P/cgi-bin/env.sh)
  d=$(rps -n $N -c $C -k http://127.0.0.1:$P/api/hello.txt)
  echo "| $m | $a req/s | $b req/s | $c req/s | $d req/s |" >> "$out"
  kill -INT $S; wait $S 2>/dev/null
done

# 동시 연결 수에 따른 변화 (pool 모드, 정적 keep-alive)
./server -p $P -r "$DOCROOT" > /dev/null 2>&1 & S=$!
sleep 0.7
{
echo
echo "## 동시 연결 수에 따른 변화 (pool, 정적 keep-alive)"
echo
echo "| 동시 연결 | 초당 요청 | p99 응답 시간 |"
echo "|---|---|---|"
} >> "$out"
for cc in 1 10 50 200 1000; do
  echo "| $cc | $(rps -n $N -c $cc -k http://127.0.0.1:$P/index.html) req/s | $(p99 -n $N -c $cc -k http://127.0.0.1:$P/index.html) ms |" >> "$out"
done
kill -INT $S; wait $S 2>/dev/null
cat "$out"
