#!/bin/sh
# stream.sh — 스트리밍 예제: 1초마다 한 줄씩. 서버가 출력이 생기는 즉시 chunked로 흘려보내는지 확인용.
printf 'Content-Type: text/plain; charset=utf-8\r\n\r\n'
for i in 1 2 3 4 5; do echo "줄 $i ($(date '+%H:%M:%S'))"; sleep 1; done
