#!/bin/bash
# start.sh — 시연용 서버 3개를 한 번에 띄운다. Ctrl+C 한 번으로 모두 종료.
#   백엔드 1 (8001), 백엔드 2 (8002), 프론트 서버 (8080: 정적 + CGI + Reverse/Forward Proxy)
cd "$(dirname "$0")/.." || exit 1
make -s || exit 1
chmod +x www/cgi-bin/*.sh demo/backend1/cgi-bin/*.sh
./server -p 8001 -b 127.0.0.1 -r demo/backend1 > /tmp/cweb_b1.log 2>&1 & B1=$!
./server -p 8002 -b 127.0.0.1 -r demo/backend2 > /tmp/cweb_b2.log 2>&1 & B2=$!
trap 'kill -INT $B1 $B2 2>/dev/null; echo; echo "백엔드 종료"' EXIT
sleep 0.3
echo "백엔드: 8001, 8002 (로그: /tmp/cweb_b1.log, /tmp/cweb_b2.log)"
echo "브라우저: http://localhost:8080/"
./server -p 8080 -b 127.0.0.1 -r www -c routes.conf -f   # -f(열린 프록시)라 내 컴퓨터에서만 접속 허용
