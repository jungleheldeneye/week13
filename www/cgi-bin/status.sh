#!/bin/sh
# status.sh — Status/Location 헤더 예제. ?go → 302 리다이렉트, 그 외 → 418 직접 지정
if [ "$QUERY_STRING" = "go" ]; then
  printf 'Location: /index.html\r\n\r\n'
else
  printf "Status: 418 I'm a teapot\r\nContent-Type: text/plain\r\n\r\n상태 코드를 스크립트가 정했어요.\n"
fi
