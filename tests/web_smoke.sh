#!/bin/bash
# Проверка страниц веб-интерфейса в headless-браузере.
# Запускает сервер на временной базе, заполняет её через API и открывает каждую страницу:
# страница должна нарисовать заголовок <h2>, не показать ошибку и не содержать текста «null».
#
# Использование: tests/web_smoke.sh <путь к bakery> <каталог web>
# Браузер берётся из $CHROME, иначе chromium или google-chrome.
set -euo pipefail

BIN=$1
WEB=$2
PORT=${PORT:-18099}
CHROME=${CHROME:-$(command -v chromium || command -v google-chrome || true)}
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
call() { curl -sf -X "$1" "$API/$2" -H 'Content-Type: application/json' -d "$3"; }
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
    "docs/$INV"
    "docs/new?type=inventory"
    "print/$INV"
    "repost"
    "repost?date=2026-10-12&time=08:00"
)

failed=0
for page in "${PAGES[@]}"; do
    dom=$("$CHROME" --headless=new --disable-gpu --no-sandbox --virtual-time-budget=3000 --dump-dom \
        "http://127.0.0.1:$PORT/#/$page" 2>/dev/null || true)
    main=$(tr -d '\n' <<<"$dom" | grep -o '<main.*</main>' || true)
    message=$(grep -oE '<div id="message"[^>]*>[^<]*' <<<"$dom" || true)
    problems=()
    grep -qE '<h2|class="form-sheet' <<<"$main" || problems+=("страница не нарисована (нет <h2> и листа формы)")
    grep -q 'hidden' <<<"$message" || problems+=("показана ошибка: ${message#*>}")
    grep -qE '>[^<]*null[^<]*<' <<<"$main" && problems+=("в тексте есть null")
    if [ ${#problems[@]} -eq 0 ]; then
        echo "ok    #/$page"
    else
        failed=1
        printf 'FAIL  #/%s: %s\n' "$page" "$(IFS=';'; echo "${problems[*]}")"
    fi
done
exit $failed
