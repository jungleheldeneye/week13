#!/bin/bash
# post.sh — POST 예제: 본문(폼 데이터)을 표준 입력에서 CONTENT_LENGTH 바이트만큼 읽는다.
body=$(head -c "${CONTENT_LENGTH:-0}")
esc() { sed 's/&/\&amp;/g; s/</\&lt;/g; s/>/\&gt;/g'; }

printf 'Content-Type: text/html; charset=utf-8\r\n\r\n'
echo '<!doctype html><meta charset="utf-8"><title>CGI POST</title>'
echo "<h1>POST 받은 내용 (${CONTENT_LENGTH:-0} bytes)</h1><table border=1>"
# "a=1&b=2" → 한 줄에 하나씩 → URL 디코딩 → 표로 출력
printf '%s\n' "$body" | tr '&' '\n' | while IFS='=' read -r k v; do
  [ -z "$k" ] && continue
  v=$(printf '%b' "$(printf '%s' "$v" | sed 's/+/ /g; s/%\([0-9A-Fa-f][0-9A-Fa-f]\)/\\x\1/g')")
  printf '<tr><td>%s</td><td>%s</td></tr>\n' "$(printf '%s' "$k" | esc)" "$(printf '%s' "$v" | esc)"
done
echo '</table><p><a href="/cgi.html">돌아가기</a></p>'
