#!/bin/bash
# hello.sh — GET 예제: ?name=값 을 읽어 인사말을 만든다.
# CGI 규칙: "헤더 줄들 + 빈 줄 + 본문"을 표준 출력으로 내보내면 서버가 응답으로 전달한다.

# QUERY_STRING("name=hyeok&x=1")에서 name 값만 꺼내고, +와 %XX를 해독
name=$(printf '%s' "$QUERY_STRING" | tr '&' '\n' | sed -n 's/^name=//p' | head -n 1)
name=$(printf '%b' "$(printf '%s' "$name" | sed 's/+/ /g; s/%\([0-9A-Fa-f][0-9A-Fa-f]\)/\\x\1/g')")
[ -z "$name" ] && name="손님"
# HTML 특수문자 이스케이프 (XSS 방지)
name=$(printf '%s' "$name" | sed 's/&/\&amp;/g; s/</\&lt;/g; s/>/\&gt;/g; s/"/\&quot;/g')

printf 'Content-Type: text/html; charset=utf-8\r\n\r\n'
cat <<HTML
<!doctype html><meta charset="utf-8"><title>CGI Hello</title>
<h1>안녕하세요, ${name}님!</h1>
<p>이 페이지는 파일이 아니라 <b>스크립트 실행 결과</b>입니다. (실행 시각: $(date '+%H:%M:%S'))</p>
<ul>
<li>REQUEST_METHOD = ${REQUEST_METHOD}</li>
<li>PATH_INFO = ${PATH_INFO:-(없음)}</li>
<li>REMOTE_ADDR = ${REMOTE_ADDR}</li>
</ul>
<p><a href="/cgi.html">돌아가기</a></p>
HTML
