#!/bin/bash
# proxy_test.sh — Reverse/Forward Proxy 통합 테스트
#   Round-Robin, strip, Host 규칙, 장애 전환(failover), 전부 다운 → 502,
#   Forward Proxy 중계 + 캐시(MISS → HIT), 프록시 루프 → 508
# 사용: make && bash tests/proxy_test.sh   (포트 8011, 8012, 8098 사용)
#   시연 서버(demo/start.sh: 8001, 8002, 8080)가 켜져 있어도 겹치지 않게 별도 포트와 임시 규칙 파일을 씀
P=8098
U=http://127.0.0.1:$P
ROUTES=$(mktemp /tmp/cweb_routes.XXXXXX)
printf 'route /api/ 127.0.0.1:8011 127.0.0.1:8012\nroute /b1/ 127.0.0.1:8011 strip\nhost api.local 127.0.0.1:8011 127.0.0.1:8012\n' > "$ROUTES"
./server -p 8011 -r demo/backend1 > /dev/null 2>&1 & B1=$!
./server -p 8012 -r demo/backend2 > /dev/null 2>&1 & B2=$!
./server -p $P -r www -c "$ROUTES" -f > /tmp/proxy_test.log 2>&1 & S=$!
trap 'kill -INT $B1 $B2 $S 2>/dev/null; rm -f "$ROUTES"' EXIT
sleep 0.7
pass=0; fail=0
check() { if [ "$2" = "$3" ]; then pass=$((pass+1)); else fail=$((fail+1)); echo "실패: $1 (기대 '$2', 실제 '$3')"; fi; }
c() { curl -s --noproxy '*' "$@"; }                       # 환경의 프록시 설정 무시
fp() { curl -s --noproxy '' --proxy "$U" "$@"; }          # 우리 서버를 Forward Proxy로 사용

# Round-Robin: 4번 요청하면 백엔드 1, 2가 2번씩
r=$(for i in 1 2 3 4; do c "$U/api/hello.txt"; done | sort | uniq -c | awk '{print $1}' | tr '\n' ' ')
check "Round-Robin 2:2 분배" "2 2 " "$r"
check "strip (/b1/api → /api)" "백엔드 1의 hello.txt" "$(c "$U/b1/api/hello.txt")"
check "Host 규칙 (api.local)" 1 "$(c -H 'Host: api.local' "$U/api/hello.txt" | grep -c '백엔드')"
check "Via 헤더 추가" 1 "$(c -D- -o /dev/null "$U/api/hello.txt" | grep -ci "via: .*cweb-$P")"
check "규칙 없는 경로는 직접 서빙" 200 "$(c -o /dev/null -w '%{http_code}' "$U/index.html")"

kill -INT $B2; sleep 0.5                                   # 백엔드 2 다운
r=$(for i in 1 2 3 4; do c -o /dev/null -w '%{http_code} ' "$U/api/hello.txt"; done)
check "백엔드 2 다운 → 모두 백엔드 1로 (failover)" "200 200 200 200 " "$r"
kill -INT $B1; sleep 0.5                                   # 둘 다 다운
check "백엔드 전부 다운 → 502" 502 "$(c -o /dev/null -w '%{http_code}' "$U/api/hello.txt")"

./server -p 8011 -r www > /dev/null 2>&1 & B1=$!            # Forward 테스트용 원 서버
sleep 0.5
check "Forward Proxy 중계" 200 "$(fp -o /dev/null -w '%{http_code}' http://127.0.0.1:8011/index.html)"
check "Forward 캐시 첫 요청 MISS" 1 "$(fp -D- -o /dev/null http://127.0.0.1:8011/cgi-bin/time.sh | grep -c 'X-Cache: MISS')"
check "Forward 캐시 두 번째 HIT" 1 "$(fp -D- -o /dev/null http://127.0.0.1:8011/cgi-bin/time.sh | grep -c 'X-Cache: HIT')"
check "no-store 응답은 저장 안 함" 0 "$(fp -s http://127.0.0.1:8011/cgi-bin/time.sh?nostore >/dev/null; fp -D- -o /dev/null http://127.0.0.1:8011/cgi-bin/time.sh?nostore | grep -c HIT)"
# 이미 이 서버를 거쳐 온 요청(Via에 cweb-포트)이 또 들어오면 무한 순환 → 508로 끊음
check "프록시 루프 감지 → 508" 508 "$(c -H "Via: 1.1 cweb-$P" -o /dev/null -w '%{http_code}' "$U/api/hello.txt")"
check "없는 호스트 → 502" 502 "$(fp -o /dev/null -w '%{http_code}' http://no-such-host.invalid/)"

echo "프록시 통합 테스트: ${pass}개 통과, ${fail}개 실패"
[ $fail -eq 0 ]
