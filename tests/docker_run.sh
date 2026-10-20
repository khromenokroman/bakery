#!/bin/bash
# Проверка Docker-образа: запуск от непривилегированного пользователя, часовой пояс, вход, запись в базу
# и резервная копия в томе /data, быстрая остановка по docker stop, данные после пересоздания контейнера.
#
# Использование: tests/docker_run.sh <образ>
set -u
IMAGE=$1
PORT=${PORT:-18094}
NAME=bakery-test-$$
VOLUME=bakery-test-$$
fail=0
ok() { echo "ok    $*"; }
bad() { echo "FAIL  $*"; fail=1; }
cleanup() { docker rm -f "$NAME" >/dev/null 2>&1; docker volume rm -f "$VOLUME" >/dev/null 2>&1; }
trap cleanup EXIT
start() { docker run -d --name "$NAME" -p "127.0.0.1:$PORT:8080" -e TZ=Europe/Kaliningrad -v "$VOLUME:/data" "$IMAGE" >/dev/null; }
wait_http() { for _ in $(seq 100); do curl -sf "http://127.0.0.1:$PORT/api/version" >/dev/null && return 0; sleep 0.1; done; return 1; }
API=http://127.0.0.1:$PORT/api

start
wait_http && ok "отвечает: $(curl -s $API/version)" || { bad "не отвечает"; docker logs "$NAME" | tail -5; exit 1; }
[ "$(docker exec "$NAME" id -u)" = 10001 ] && ok "работает не от root (uid 10001)" || bad "пользователь: $(docker exec "$NAME" id)"
[ "$(docker exec "$NAME" date +%Z)" = EET ] && ok "часовой пояс из TZ" || bad "часовой пояс: $(docker exec "$NAME" date)"
COOKIE=$(curl -sf -D - -o /dev/null -X POST "$API/auth/setup" -H 'Content-Type: application/json' \
    -d '{"login":"admin","password":"secret1"}' | sed -n 's/^Set-Cookie: \([^;]*\).*/\1/Ip')
[ -n "$COOKIE" ] && ok "создан администратор" || bad "не создан администратор"
curl -sf -X POST "$API/items" -H 'Content-Type: application/json' -H "Cookie: $COOKIE" \
    -d '{"name":"Батон","kind":"product","unit":"pcs"}' >/dev/null && ok "запись в базу" || bad "запись в базу"
curl -sf -X POST "$API/backups" -H "Cookie: $COOKIE" >/dev/null && docker exec "$NAME" sh -c 'ls /data/backups/bakery-*-manual.db' >/dev/null \
    && ok "резервная копия в /data/backups" || bad "резервная копия"
curl -sf "http://127.0.0.1:$PORT/" | grep -q '<title>' && ok "веб-страница отдаётся" || bad "нет страницы"

begin=$(date +%s)
docker stop -t 10 "$NAME" >/dev/null
spent=$(($(date +%s) - begin))
code=$(docker inspect -f '{{.State.ExitCode}}' "$NAME")
[ "$spent" -lt 5 ] && [ "$code" = 0 ] && ok "docker stop: остановлен за ${spent} с, код 0" || bad "docker stop: ${spent} с, код $code"
docker rm "$NAME" >/dev/null

start
wait_http || bad "после пересоздания не отвечает"
curl -s -H "Cookie: $COOKIE" "$API/items" | grep -q Батон && ok "данные и сессия на месте после пересоздания контейнера" \
    || bad "после пересоздания: $(curl -s -H "Cookie: $COOKIE" "$API/items")"
exit $fail
