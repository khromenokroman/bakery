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
    call() { curl -sf -X "$1" "$API/$2" -H 'Content-Type: application/json' -d "$3"; }
    id() { sed -n 's/.*"id":\([0-9]*\).*/\1/p'; }
    BREAD=$(call POST items '{"name":"Батон","kind":"product","unit":"pcs"}' | id)
    CAKE=$(call POST items '{"name":"Торт","kind":"product","unit":"kg"}' | id)
    call POST docs "{\"type\":\"order\",\"date\":\"2026-10-12\",\"comment\":\"Колосок\",\"lines\":[{\"item_id\":$BREAD,\"qty\":40000},{\"item_id\":$CAKE,\"qty\":5000}]}" >/dev/null
    call POST docs "{\"type\":\"order\",\"date\":\"2026-10-12\",\"comment\":\"Петрова\",\"lines\":[{\"item_id\":$BREAD,\"qty\":5000}]}" >/dev/null

    if node "$DIR/e2e/drive.js" "http://127.0.0.1:$PORT/$hash" "$DIR/e2e/$script"; then
        echo "ok    $script"
    else
        echo "FAIL  $script"
        failed=1
    fi
    kill $PID; wait $PID 2>/dev/null || true
    rm -rf "$TMP"
done
exit $failed
