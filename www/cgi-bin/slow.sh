#!/bin/sh
# slow.sh — 시간 초과 시연: 15초 걸리는 스크립트 → 서버가 10초에 강제 종료하고 504 응답
sleep 15
printf 'Content-Type: text/plain\r\n\r\n너무 늦었어요\n'
