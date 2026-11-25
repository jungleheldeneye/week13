#!/bin/sh
# env.sh — 서버가 스크립트에 넘겨준 환경 변수(CGI 메타 변수)를 전부 보여준다. (학습/디버깅용)
printf 'Content-Type: text/plain; charset=utf-8\r\n\r\n'
env | sort
