#!/bin/sh
# cat.sh — 받은 본문을 그대로 돌려준다 (큰 POST 본문이 막힘 없이 오가는지 확인용)
printf "Content-Type: application/octet-stream\r\n\r\n"
cat
