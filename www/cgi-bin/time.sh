#!/bin/sh
# time.sh — 캐시 시연용: 현재 시각을 보여준다. Forward Proxy로 두 번 요청하면
#   두 번째는 캐시(X-Cache: HIT)에서 와서 시각이 그대로다.
#   ?maxage=N → Cache-Control: max-age=N (N초 뒤 만료),  ?nostore → 저장 금지
case "$QUERY_STRING" in
  maxage=*) cc="max-age=${QUERY_STRING#maxage=}" ;;
  nostore)  cc="no-store" ;;
  *)        cc="max-age=30" ;;
esac
printf 'Content-Type: text/plain; charset=utf-8\r\nCache-Control: %s\r\n\r\n' "$cc"
echo "원 서버가 만든 시각: $(date '+%H:%M:%S')  (Cache-Control: $cc)"
