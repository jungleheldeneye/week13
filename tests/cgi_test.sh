#!/bin/bash
# cgi_test.sh — CGI 통합 테스트: 서버를 띄우고 curl로 요청해 상태 코드/내용을 확인한다.
# 사용: make && bash tests/cgi_test.sh   (slow.sh 504 검사 때문에 약 10초 걸림)
PORT=${PORT:-8099}
U=http://127.0.0.1:$PORT
./server -p $PORT -r www > /tmp/cgi_test.log 2>&1 &
SRV=$!
trap 'kill -INT $SRV 2>/dev/null' EXIT
sleep 0.5
pass=0; fail=0
check() {  # check "설명" 기대값 실제값
  if [ "$2" = "$3" ]; then pass=$((pass+1)); else fail=$((fail+1)); echo "실패: $1 (기대 '$2', 실제 '$3')"; fi
}
code() { command curl -s --noproxy '*' -o /dev/null -w '%{http_code}' "$@"; }
# md5: Linux는 md5sum, macOS는 md5 -q
md5() { if command -v md5sum >/dev/null; then md5sum | cut -c1-32; else command md5 -q; fi; }
curl() { command curl --noproxy '*' "$@"; }   # 환경에 설정된 프록시를 거치지 않게

check "GET + 쿼리 해독"      1   "$(curl -s "$U/cgi-bin/hello.sh?name=%ED%98%81" | grep -c '혁님')"
check "XSS 이스케이프"       1   "$(curl -s "$U/cgi-bin/hello.sh?name=%3Cb%3E" | grep -c '&lt;b&gt;')"
check "PATH_INFO"           1   "$(curl -s "$U/cgi-bin/hello.sh/a/b" | grep -c 'PATH_INFO = /a/b')"
check "POST 폼"             1   "$(curl -s -d 'memo=a%26b' "$U/cgi-bin/post.sh" | grep -c 'a&amp;b')"
check "Status 헤더"         418 "$(code "$U/cgi-bin/status.sh")"
check "Location → 302"      302 "$(code "$U/cgi-bin/status.sh?go")"
check "HEAD"                200 "$(code -I "$U/cgi-bin/hello.sh")"
check "헤더 없는 출력 → 502"  502 "$(code "$U/cgi-bin/bad.sh")"
check "실행 권한 없음 → 403"  403 "$(code "$U/cgi-bin/noexec.txt")"
check "없는 스크립트 → 404"   404 "$(code "$U/cgi-bin/nope.sh")"
check "폴더 자체 → 403"       403 "$(code "$U/cgi-bin/")"
check "경로 탈출 → 403"       403 "$(code --path-as-is "$U/cgi-bin/%2e%2e/index.html")"
check "httpoxy 차단"         0   "$(curl -s -H 'Proxy: http://evil' "$U/cgi-bin/env.sh" | grep -c HTTP_PROXY)"
check "HTTP/1.1 chunked"    1   "$(curl -s -D- -o /dev/null "$U/cgi-bin/env.sh" | grep -ci 'transfer-encoding: chunked')"
check "keep-alive 재사용"    1   "$(curl -s -v "$U/cgi-bin/env.sh" "$U/cgi-bin/env.sh" -o /dev/null -o /dev/null 2>&1 | grep -c 'Re-using')"
head -c 3000000 /dev/urandom > /tmp/cgi_big
check "3MB POST 왕복 (교착 없음)" "$(md5 < /tmp/cgi_big)" \
      "$(curl -s --data-binary @/tmp/cgi_big "$U/cgi-bin/cat.sh" | md5)"
check "10초 초과 → 504"      504 "$(code "$U/cgi-bin/slow.sh")"
echo "CGI 통합 테스트: ${pass}개 통과, ${fail}개 실패"
[ $fail -eq 0 ]
