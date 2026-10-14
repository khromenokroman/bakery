#!/bin/bash
# Сценарии с кликами в headless Chrome (tests/e2e/*.js через tests/e2e/drive.js, нужен node).
# Каждый сценарий получает свежую базу с батоном (шт), тортом (кг) и двумя заявками на 2026-10-12.
#
# Использование: tests/web_e2e.sh <путь к bakery> <каталог web>
set -euo pipefail

BIN=$1
WEB=$2
PORT=${PORT:-18098}
DIR=$(cd "$(dirname "$0")" && pwd)
export CHROME=${CHROME:-$(command -v chromium || command -v google-chrome || true)}
[ -n "$CHROME" ] || { echo "web_e2e: не найден chromium или google-chrome" >&2; exit 2; }

# сценарий и адрес, с которого он начинается
SCENARIOS=(
    "production_fill.js|#/docs/new?type=production&date=2026-10-12"
    "order_new.js|#/orders/new?date=2026-10-13"
    "groups.js|#/items"
    "prices.js|#/prices"
    "sale.js|#/docs"
    "return_writeoff.js|#/docs"
    "settings_print.js|#/docs"
    "report.js|#/docs"
    "inventory.js|#/docs"
    "repost.js|#/docs"
    "period.js|#/docs"
    "doc_filter.js|#/docs?from=2026-10-01&to=2026-10-31"
    "item_photo.js|#/items"
    "users.js|#/items"
    "backups.js|#/items"
    "demo.js|#/"
)

failed=0
for entry in "${SCENARIOS[@]}"; do
    script=${entry%%|*}
    hash=${entry#*|}
    TMP=$(mktemp -d)
    "$BIN" --db "$TMP/e2e.db" --web "$WEB" --port "$PORT" >"$TMP/server.log" 2>&1 &
    PID=$!
    for _ in $(seq 50); do
        curl -sf "http://127.0.0.1:$PORT/api/version" >/dev/null && break
        sleep 0.1
    done
    API=http://127.0.0.1:$PORT/api
    if [ "$script" = demo.js ]; then
        # демонстрационная база создаётся из пустой: без начальных данных, заполнение - до минуты
        if E2E_TIMEOUT=180 node "$DIR/e2e/drive.js" "http://127.0.0.1:$PORT/$hash" "$DIR/e2e/$script"; then
            echo "ok    $script"
        else
            echo "FAIL  $script"
            failed=1
        fi
        kill $PID; wait $PID 2>/dev/null || true
        rm -rf "$TMP"
        continue
    fi
    # сценарии идут под администратором (кроме users.js: он входит сам)
    ADMIN=$(curl -sf -D - -o /dev/null -X POST "$API/auth/setup" -H 'Content-Type: application/json' \
        -d '{"login":"admin","name":"Админ","password":"secret1"}' | sed -n 's/^Set-Cookie: \([^;]*\).*/\1/Ip')
    call() { curl -sf -X "$1" "$API/$2" -H 'Content-Type: application/json' -H "Cookie: $ADMIN" -d "$3"; }
    id() { sed -n 's/.*"id":\([0-9]*\).*/\1/p'; }
    BREAD=$(call POST items '{"name":"Батон","kind":"product","unit":"pcs"}' | id)
    CAKE=$(call POST items '{"name":"Торт","kind":"product","unit":"kg"}' | id)
    call POST docs "{\"type\":\"order\",\"date\":\"2026-10-12\",\"comment\":\"Колосок\",\"lines\":[{\"item_id\":$BREAD,\"qty\":40000},{\"item_id\":$CAKE,\"qty\":5000}]}" >/dev/null
    call POST docs "{\"type\":\"order\",\"date\":\"2026-10-12\",\"comment\":\"Петрова\",\"lines\":[{\"item_id\":$BREAD,\"qty\":5000}]}" >/dev/null

    cookie=$ADMIN
    [ "$script" = users.js ] && cookie=
    if COOKIE=$cookie node "$DIR/e2e/drive.js" "http://127.0.0.1:$PORT/$hash" "$DIR/e2e/$script"; then
        echo "ok    $script"
    else
        echo "FAIL  $script"
        failed=1
    fi
    kill $PID; wait $PID 2>/dev/null || true
    rm -rf "$TMP"
done
exit $failed
