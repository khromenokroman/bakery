#!/bin/bash
# Проверка страниц веб-интерфейса в headless-браузере.
# Запускает сервер на временной базе, заполняет её через API и открывает каждую страницу:
# страница должна нарисовать заголовок <h2>, не показать ошибку и не содержать текста «null».
#
# Страницы открываются под администратором, часть ещё под продавцом (SELLER_PAGES) и без входа.
#
# Использование: tests/web_smoke.sh <путь к bakery> <каталог web>
# Браузер берётся из $CHROME, иначе chromium или google-chrome; нужен node (tests/e2e/drive.js).
set -euo pipefail

BIN=$1
WEB=$2
PORT=${PORT:-18099}
export CHROME=${CHROME:-$(command -v chromium || command -v google-chrome || true)}
DIR=$(cd "$(dirname "$0")" && pwd)
if [ -z "$CHROME" ]; then
    echo "web_smoke: не найден chromium или google-chrome" >&2
    exit 2
fi

TMP=$(mktemp -d)
"$BIN" --db "$TMP/smoke.db" --web "$WEB" --port "$PORT" >"$TMP/server.log" 2>&1 &
PID=$!
trap 'kill $PID 2>/dev/null || true; wait $PID 2>/dev/null || true; rm -rf "$TMP"' EXIT
for _ in $(seq 50); do
    curl -sf "http://127.0.0.1:$PORT/api/version" >/dev/null && break
    sleep 0.1
done

API=http://127.0.0.1:$PORT/api
# без входа: страница первого запуска
dump_page() { COOKIE=$1 node "$DIR/e2e/drive.js" "http://127.0.0.1:$PORT/#/$2" --dump 2>/dev/null || true; }
SETUP_DOM=$(dump_page "" items)
# администратор, его сессия для curl и браузера
ADMIN=$(curl -sf -D - -o /dev/null -X POST "$API/auth/setup" -H 'Content-Type: application/json' \
    -d '{"login":"admin","name":"Админ","password":"secret1"}' | sed -n 's/^Set-Cookie: \([^;]*\).*/\1/Ip')
call() { curl -sf -X "$1" "$API/$2" -H 'Content-Type: application/json' -H "Cookie: $ADMIN" -d "$3"; }
id() { sed -n 's/.*"id":\([0-9]*\).*/\1/p'; }

PASTRY=$(call POST groups '{"name":"Кондитерка"}' | id)
CAKES=$(call POST groups "{\"name\":\"Торты\",\"parent_id\":$PASTRY}" | id)
FLOUR=$(call POST items '{"name":"Мука","kind":"raw","unit":"kg"}' | id)
EGG=$(call POST items '{"name":"Яйцо","kind":"raw","unit":"pcs"}' | id)
DOUGH=$(call POST items '{"name":"Тесто","kind":"semi","unit":"kg"}' | id)
BREAD=$(call POST items '{"name":"Батон","kind":"product","unit":"pcs"}' | id)
COOKIE=$(call POST items "{\"name\":\"Печенье\",\"kind\":\"product\",\"unit\":\"kg\",\"group_id\":$CAKES}" | id)
call PUT "items/$DOUGH/recipe" "{\"output_qty\":1000,\"lines\":[{\"component_id\":$FLOUR,\"qty\":600}]}" >/dev/null
call PUT "items/$BREAD/recipe" "{\"output_qty\":1000,\"lines\":[{\"component_id\":$DOUGH,\"qty\":450},{\"component_id\":$EGG,\"qty\":100}]}" >/dev/null
O1=$(call POST docs "{\"type\":\"order\",\"date\":\"2026-10-12\",\"lines\":[{\"item_id\":$BREAD,\"qty\":10000}]}" | id)
P1=$(call POST docs "{\"type\":\"production\",\"date\":\"2026-10-12\",\"lines\":[{\"item_id\":$BREAD,\"qty\":10000}]}" | id)
P2=$(call POST docs "{\"type\":\"production\",\"date\":\"2026-10-12\",\"lines\":[{\"item_id\":$COOKIE,\"qty\":1000}]}" | id)
call POST "docs/$P2/post" '' >/dev/null
V1=$(call POST price-types '{"name":"Вариант 1"}' | id)
PR=$(call POST docs "{\"type\":\"prices\",\"date\":\"2026-10-10\",\"price_type_id\":1,\"lines\":[{\"item_id\":$BREAD,\"price_kop\":3120}]}" | id)
call POST "docs/$PR/post" '' >/dev/null
SHOP=$(call POST partners '{"name":"Колосок","phone":"+7 900"}' | id)
SALE=$(call POST docs "{\"type\":\"sale\",\"date\":\"2026-10-12\",\"price_type_id\":$V1,\"partner_id\":$SHOP,\"lines\":[{\"item_id\":$COOKIE,\"qty\":500,\"price_kop\":52000},{\"item_id\":$BREAD,\"qty\":1000}]}" | id)
call POST "docs/$P1/post" '' >/dev/null
SALE2=$(call POST docs "{\"type\":\"sale\",\"date\":\"2026-10-12\",\"price_type_id\":$V1,\"partner_id\":$SHOP,\"lines\":[{\"item_id\":$BREAD,\"qty\":4000,\"price_kop\":4500}]}" | id)
call POST "docs/$SALE2/post" '' >/dev/null
RET=$(call POST docs "{\"type\":\"return\",\"date\":\"2026-10-13\",\"base_id\":$SALE2,\"lines\":[{\"item_id\":$BREAD,\"qty\":1000}]}" | id)
WO=$(call POST docs "{\"type\":\"writeoff\",\"date\":\"2026-10-13\",\"comment\":\"брак\",\"lines\":[{\"item_id\":$BREAD,\"qty\":1000}]}" | id)
INV=$(call POST docs "{\"type\":\"inventory\",\"date\":\"2026-10-14\",\"time\":\"20:00\",\"group_id\":$CAKES,\"lines\":[{\"item_id\":$COOKIE,\"qty\":200}]}" | id)
O2=$(call POST docs "{\"type\":\"order\",\"date\":\"2026-10-12\",\"comment\":\"Магазин\",\"lines\":[{\"item_id\":$COOKIE,\"qty\":1500}]}" | id)
# закрытый период: заявка и установка цен 01.10 только для просмотра
OLD=$(call POST docs "{\"type\":\"order\",\"date\":\"2026-10-01\",\"lines\":[{\"item_id\":$BREAD,\"qty\":1000}]}" | id)
PR0=$(call POST docs "{\"type\":\"prices\",\"date\":\"2026-10-01\",\"price_type_id\":$V1,\"lines\":[{\"item_id\":$BREAD,\"price_kop\":4000}]}" | id)
call POST "docs/$PR0/post" '' >/dev/null
call PUT period '{"closed_till":"2026-10-05"}' >/dev/null
# фото батона: PNG 1x1
base64 -d <<<'iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mP8z8BQDwAEhQGAhKmMIQAAAABJRU5ErkJggg==' \
    | curl -sf -X PUT "$API/items/$BREAD/photo" -H 'Content-Type: image/png' -H "Cookie: $ADMIN" --data-binary @- >/dev/null
call POST users '{"login":"olga","name":"Ольга","role":"seller","password":"secret1"}' >/dev/null
call POST backups '' >/dev/null
# незавершённый прогон перепроведения: страница #/repost показывает «Продолжить»
call POST repost/step '{"date":"2026-10-12","time":"00:00"}' >/dev/null
SELLER=$(curl -sf -D - -o /dev/null -X POST "$API/auth/login" -H 'Content-Type: application/json' \
    -d '{"login":"olga","password":"secret1"}' | sed -n 's/^Set-Cookie: \([^;]*\).*/\1/Ip')

PAGES=(
    "items"
    "items/$FLOUR"
    "items/$BREAD"
    "orders?date=2026-10-12"
    "orders?date=2030-01-01"
    "orders/new"
    "orders/$O1"
    "requirements?date=2026-10-12&docs=$O1"
    "requirements?date=2026-10-12&docs=$O1,$O2"
    "docs"
    "docs?from=2026-10-01&to=2026-10-31&types=sale,return"
    "docs/new?type=production"
    "docs/$P1"
    "docs/$P2"
    "stock?date=2026-10-12"
    "stock?date=2026-10-01"
    "stock?date=2026-10-12&group=$PASTRY"
    "items/$COOKIE"
    "prices"
    "prices?type=$V1&date=2026-10-12"
    "docs/$PR"
    "docs/new?type=prices&price_type=$V1"
    "docs/$SALE"
    "docs/new?type=sale"
    "partners"
    "partners/$SHOP"
    "docs/$SALE2"
    "docs/$RET"
    "docs/new?type=return&base=$SALE2"
    "docs/$WO"
    "docs/new?type=writeoff"
    "settings"
    "print/$SALE2"
    "print/$WO"
    "print/$PR"
    "print/$O1"
    "print/$RET"
    "print/$P1"
    "print/$SALE2?form=torg12"
    "print/$SALE2?form=upd"
    "report?from=2026-10-01&to=2026-10-31"
    "report?from=2026-10-12&to=2026-10-12&group=$PASTRY"
    "sales?from=2026-10-01&to=2026-10-31"
    "sales?from=2026-10-12&to=2026-10-13&group=$PASTRY"
    "docs/$INV"
    "docs/new?type=inventory"
    "print/$INV"
    "repost"
    "repost?date=2026-10-12&time=08:00"
    "orders/$OLD"
    "docs/$PR0"
    "users"
    "password"
    "backups"
)
SELLER_PAGES=(
    "items"
    "items/$BREAD"
    "docs"
    "docs/$SALE2"
    "settings"
    "repost"
    "sales?from=2026-10-12&to=2026-10-13"
    "password"
)

failed=0
check() {
    local who=$1 cookie=$2 page=$3
    dom=$(dump_page "$cookie" "$page")
    main=$(tr -d '\n' <<<"$dom" | grep -o '<main.*</main>' || true)
    message=$(grep -oE '<div id="message"[^>]*>[^<]*' <<<"$dom" || true)
    problems=()
    grep -qE '<h2|class="form-sheet' <<<"$main" || problems+=("страница не нарисована (нет <h2> и листа формы)")
    grep -q 'hidden' <<<"$message" || problems+=("показана ошибка: ${message#*>}")
    grep -q 'class="login"' <<<"$main" && problems+=("показана форма входа")
    grep -qE '>[^<]*null[^<]*<' <<<"$main" && problems+=("в тексте есть null")
    if [ ${#problems[@]} -eq 0 ]; then
        echo "ok    $who #/$page"
    else
        failed=1
        printf 'FAIL  %s #/%s: %s\n' "$who" "$page" "$(IFS=';'; echo "${problems[*]}")"
    fi
}
if grep -q 'Создать администратора' <<<"$SETUP_DOM"; then
    echo "ok    пустая база: форма первого запуска"
else
    failed=1
    echo "FAIL  пустая база: нет формы первого запуска"
fi
for page in "${PAGES[@]}"; do
    check admin "$ADMIN" "$page"
done
for page in "${SELLER_PAGES[@]}"; do
    check seller "$SELLER" "$page"
done
if grep -q '>Войти<' <<<"$(dump_page "" items)"; then
    echo "ok    без входа: форма входа"
else
    failed=1
    echo "FAIL  без входа: нет формы входа"
fi
exit $failed
