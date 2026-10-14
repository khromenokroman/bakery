#!/bin/bash
# Сценарий внутри контейнера с systemd (запускает tests/deb_install.sh): установка пакета из /deb,
# служба, обновление с сохранением настроек и данных, выключенная служба, remove, purge, повторная установка.
set -u
fail=0
ok() { echo "ok    $*"; }
bad() { echo "FAIL  $*"; fail=1; }
wait_http() { for _ in $(seq 50); do curl -sf "http://127.0.0.1:$1/api/version" >/dev/null && return 0; sleep 0.2; done; return 1; }
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq >/dev/null
apt-get install -y -qq /deb/*.deb >/dev/null 2>&1 || bad "apt install"
systemctl is-active -q bakery && ok "служба запущена" || { bad "служба не запущена"; journalctl -u bakery --no-pager | tail -5; }
systemctl is-enabled -q bakery && ok "служба включена" || bad "служба не включена"
wait_http 8080 && ok "отвечает на 8080: $(curl -s http://127.0.0.1:8080/api/version)" || bad "нет ответа на 8080"
ss -ltn | grep -q '0.0.0.0:8080' && ok "слушает 0.0.0.0:8080" || bad "не 0.0.0.0: $(ss -ltn | grep 8080)"
[ "$(systemctl show -p User --value bakery)" = bakery ] && ok "под пользователем bakery" || bad "пользователь службы"
[ "$(stat -c %U:%a /var/lib/bakery)" = bakery:750 ] && ok "/var/lib/bakery bakery 750" || bad "права каталога: $(stat -c %U:%a /var/lib/bakery)"
curl -sf http://127.0.0.1:8080/ | grep -q '<title>' && ok "веб-страница отдаётся" || bad "нет страницы"
curl -sf http://127.0.0.1:8080/api/auth | grep -q '"setup":true' && ok "новая база ждёт администратора" || bad "нет первого запуска"
# сессия хранится в базе, поэтому cookie действует и после обновлений и переустановки
COOKIE=$(curl -sf -D - -o /dev/null -X POST http://127.0.0.1:8080/api/auth/setup -H 'Content-Type: application/json' \
    -d '{"login":"admin","password":"secret1"}' | sed -n 's/^Set-Cookie: \([^;]*\).*/\1/Ip')
[ -n "$COOKIE" ] && ok "создан администратор" || bad "не создан администратор"
items() { curl -s -H "Cookie: $COOKIE" "http://127.0.0.1:$1/api/items"; }
curl -sf -X POST http://127.0.0.1:8080/api/items -H 'Content-Type: application/json' -H "Cookie: $COOKIE" \
    -d '{"name":"Батон","kind":"product","unit":"pcs"}' >/dev/null && ok "запись в базу" || bad "запись в базу"
curl -sf -X POST http://127.0.0.1:8080/api/backups -H "Cookie: $COOKIE" >/dev/null \
    && [ "$(stat -c %U /var/lib/bakery/backups/bakery-*-manual.db 2>/dev/null)" = bakery ] \
    && ok "резервная копия в /var/lib/bakery/backups" || bad "резервная копия: $(ls -la /var/lib/bakery/backups 2>&1)"

sed -i 's/--port 8080/--port 8081/' /etc/default/bakery
apt-get install -y -qq --reinstall /deb/*.deb >/dev/null 2>&1 || bad "переустановка"
grep -q 8081 /etc/default/bakery && ok "настройки сохранены при обновлении" || bad "настройки затёрты"
wait_http 8081 && ok "после обновления перезапущена на 8081" || bad "нет ответа на 8081"
[ "$(items 8081 | grep -c Батон)" = 1 ] && ok "данные на месте после обновления" || bad "данные пропали"

systemctl disable -q --now bakery
apt-get install -y -qq --reinstall /deb/*.deb >/dev/null 2>&1
systemctl is-enabled -q bakery && bad "выключенная служба включилась при обновлении" || ok "выключенная администратором остаётся выключенной"
systemctl enable -q --now bakery

apt-get remove -y -qq bakery >/dev/null 2>&1 || bad "remove"
systemctl is-active -q bakery && bad "служба работает после удаления" || ok "remove: служба остановлена"
[ -f /var/lib/bakery/bakery.db ] && ok "remove: база осталась" || bad "remove: база удалена"
apt-get purge -y -qq bakery >/dev/null 2>&1 || bad "purge"
[ ! -e /etc/default/bakery ] && ok "purge: настройки удалены" || bad "purge: настройки остались"
[ -f /var/lib/bakery/bakery.db ] && ok "purge: база осталась" || bad "purge: база удалена"

apt-get install -y -qq /deb/*.deb >/dev/null 2>&1 || bad "повторная установка"
wait_http 8080 && [ "$(items 8080 | grep -c Батон)" = 1 ] && ok "повторная установка: служба и прежние данные" || bad "повторная установка"

# сброс к чистому состоянию по README
systemctl stop bakery
sh -c 'cd /var/lib/bakery && d=trial-$(date +%F-%H%M%S) && mkdir $d && mv bakery.db* backups $d/ 2>/dev/null; ls $d' >/dev/null
systemctl start bakery
wait_http 8080 && curl -sf http://127.0.0.1:8080/api/auth | grep -q '"setup":true' && ok "сброс: пустая база ждёт администратора" || bad "сброс: нет первого запуска"
ls /var/lib/bakery/trial-*/bakery.db /var/lib/bakery/trial-*/backups >/dev/null 2>&1 && ok "сброс: пробные база и копии отложены" || bad "сброс: пробные данные не сохранены: $(ls -R /var/lib/bakery)"
exit $fail
