#include "schema.hpp"

#include <algorithm>
#include <array>
#include <string>
#include <string_view>

namespace {

/// Миграции схемы. Применённую миграцию не менять: изменения только новой миграцией в конце.
constexpr std::array<std::string_view, 7> MIGRATIONS = {
    // 1: справочники, документы, движения склада
    R"sql(
CREATE TABLE items (
    id       INTEGER PRIMARY KEY,
    name     TEXT    NOT NULL UNIQUE,
    kind     TEXT    NOT NULL CHECK (kind IN ('raw', 'semi', 'product')),
    unit     TEXT    NOT NULL CHECK (unit IN ('kg', 'l', 'pcs')),
    archived INTEGER NOT NULL DEFAULT 0 CHECK (archived IN (0, 1))
);

CREATE TABLE recipes (
    item_id    INTEGER PRIMARY KEY REFERENCES items (id) ON DELETE CASCADE,
    output_qty INTEGER NOT NULL CHECK (output_qty > 0)
);

CREATE TABLE recipe_lines (
    item_id      INTEGER NOT NULL REFERENCES recipes (item_id) ON DELETE CASCADE,
    component_id INTEGER NOT NULL REFERENCES items (id),
    qty          INTEGER NOT NULL CHECK (qty > 0),
    PRIMARY KEY (item_id, component_id),
    CHECK (item_id <> component_id)
);

CREATE TABLE prices (
    item_id   INTEGER NOT NULL REFERENCES items (id) ON DELETE CASCADE,
    date_from TEXT    NOT NULL,
    price_kop INTEGER NOT NULL CHECK (price_kop >= 0),
    PRIMARY KEY (item_id, date_from)
);

CREATE TABLE partners (
    id       INTEGER PRIMARY KEY,
    name     TEXT    NOT NULL UNIQUE,
    phone    TEXT    NOT NULL DEFAULT '',
    note     TEXT    NOT NULL DEFAULT '',
    archived INTEGER NOT NULL DEFAULT 0 CHECK (archived IN (0, 1))
);

CREATE TABLE users (
    id        INTEGER PRIMARY KEY,
    login     TEXT NOT NULL UNIQUE,
    pass_hash TEXT NOT NULL,
    role      TEXT NOT NULL CHECK (role IN ('technologist', 'seller'))
);

CREATE TABLE docs (
    id         INTEGER PRIMARY KEY,
    type       TEXT    NOT NULL CHECK (type IN ('order', 'production', 'sale', 'return', 'writeoff', 'inventory')),
    number     INTEGER NOT NULL,
    date       TEXT    NOT NULL,
    partner_id INTEGER REFERENCES partners (id),
    status     TEXT    NOT NULL DEFAULT 'draft' CHECK (status IN ('draft', 'posted')),
    comment    TEXT    NOT NULL DEFAULT '',
    user_id    INTEGER REFERENCES users (id),
    UNIQUE (type, number)
);

CREATE TABLE doc_lines (
    id        INTEGER PRIMARY KEY,
    doc_id    INTEGER NOT NULL REFERENCES docs (id) ON DELETE CASCADE,
    item_id   INTEGER NOT NULL REFERENCES items (id),
    qty       INTEGER NOT NULL,
    price_kop INTEGER,
    qty_fact  INTEGER
);
CREATE INDEX doc_lines_doc ON doc_lines (doc_id);

CREATE TABLE stock_moves (
    id      INTEGER PRIMARY KEY,
    doc_id  INTEGER NOT NULL REFERENCES docs (id) ON DELETE CASCADE,
    item_id INTEGER NOT NULL REFERENCES items (id),
    qty     INTEGER NOT NULL,
    date    TEXT    NOT NULL
);
CREATE INDEX stock_moves_item ON stock_moves (item_id, date);
CREATE INDEX stock_moves_doc ON stock_moves (doc_id);
)sql",
    // 2: группы номенклатуры (дерево)
    R"sql(
CREATE TABLE groups (
    id        INTEGER PRIMARY KEY,
    name      TEXT    NOT NULL,
    parent_id INTEGER REFERENCES groups (id)
);
CREATE INDEX groups_parent ON groups (parent_id);

ALTER TABLE items ADD COLUMN group_id INTEGER REFERENCES groups (id);
CREATE INDEX items_group ON items (group_id);
)sql",
    // 3: типы цен, документ «Установка цен», себестоимость в строках.
    // CHECK у docs.type в SQLite не меняется, поэтому таблица пересобирается (внешние ключи выключены в migrate)
    R"sql(
CREATE TABLE price_types (
    id     INTEGER PRIMARY KEY,
    name   TEXT    NOT NULL UNIQUE,
    system INTEGER NOT NULL DEFAULT 0 CHECK (system IN (0, 1))
);
INSERT INTO price_types (id, name, system) VALUES (1, 'Себестоимость', 1);

DROP TABLE prices;

CREATE TABLE docs_new (
    id            INTEGER PRIMARY KEY,
    type          TEXT    NOT NULL CHECK (type IN ('order', 'production', 'sale', 'return', 'writeoff', 'inventory', 'prices')),
    number        INTEGER NOT NULL,
    date          TEXT    NOT NULL,
    partner_id    INTEGER REFERENCES partners (id),
    price_type_id INTEGER REFERENCES price_types (id),
    status        TEXT    NOT NULL DEFAULT 'draft' CHECK (status IN ('draft', 'posted')),
    comment       TEXT    NOT NULL DEFAULT '',
    user_id       INTEGER REFERENCES users (id),
    UNIQUE (type, number)
);
INSERT INTO docs_new (id, type, number, date, partner_id, status, comment, user_id)
    SELECT id, type, number, date, partner_id, status, comment, user_id FROM docs;
DROP TABLE docs;
ALTER TABLE docs_new RENAME TO docs;
CREATE INDEX docs_date ON docs (date);

ALTER TABLE doc_lines ADD COLUMN cost_kop INTEGER;
CREATE INDEX doc_lines_item ON doc_lines (item_id);
)sql",
    // 4: покрывающий индекс движений: остатки и проверка минуса читают только индекс, без строк таблицы
    // (на 800 тыс. движений отчёт по остаткам 1,5 с -> 0,18 с)
    R"sql(
CREATE INDEX stock_moves_cover ON stock_moves (item_id, date, qty);
DROP INDEX stock_moves_item;
)sql",
    // 5: документ-основание (возврат ссылается на реализацию)
    R"sql(
ALTER TABLE docs ADD COLUMN base_id INTEGER REFERENCES docs (id);
CREATE INDEX docs_base ON docs (base_id);
)sql",
    // 6: дата документа и движений с временем "ГГГГ-ММ-ДД ЧЧ:ММ:СС"; у старых 00:00:00
    R"sql(
UPDATE docs SET date = date || ' 00:00:00' WHERE length(date) = 10;
UPDATE stock_moves SET date = date || ' 00:00:00' WHERE length(date) = 10;
)sql",
    // 7: реквизиты организации (ключ-значение), реквизиты клиентов, ставка НДС товара (NULL - без НДС)
    R"sql(
CREATE TABLE settings (
    key   TEXT PRIMARY KEY,
    value TEXT NOT NULL
);
ALTER TABLE partners ADD COLUMN inn TEXT NOT NULL DEFAULT '';
ALTER TABLE partners ADD COLUMN kpp TEXT NOT NULL DEFAULT '';
ALTER TABLE partners ADD COLUMN address TEXT NOT NULL DEFAULT '';
ALTER TABLE items ADD COLUMN vat_rate INTEGER CHECK (vat_rate IN (0, 5, 7, 10, 20, 22));
)sql",
};

} // namespace

int schema_version() { return static_cast<int>(MIGRATIONS.size()); }

int current_schema_version(Database &db) {
    auto stmt = db.prepare("PRAGMA user_version");
    stmt.step();
    return static_cast<int>(stmt.column_int(0));
}

int migrate(Database &db, int target) {
    auto const current = current_schema_version(db);
    if (current > schema_version()) {
        throw DbError("база создана более новой версией программы: схема " + std::to_string(current) + ", поддерживается " +
                      std::to_string(schema_version()));
    }
    target = std::min(target, schema_version());
    if (current >= target) {
        return current;
    }
    // процедура изменения схемы SQLite: внешние ключи выключаются вне транзакции, чтобы пересборка
    // таблицы (DROP + RENAME) не запускала каскадное удаление; целостность проверяется перед COMMIT
    db.exec("PRAGMA foreign_keys = OFF");
    try {
        for (auto v = current; v < target; ++v) {
            Transaction tx{db};
            db.exec(MIGRATIONS[static_cast<std::size_t>(v)]);
            auto check = db.prepare("PRAGMA foreign_key_check");
            if (check.step()) {
                throw DbError("миграция " + std::to_string(v + 1) + " нарушила ссылочную целостность в таблице " + check.column_text(0));
            }
            db.exec("PRAGMA user_version = " + std::to_string(v + 1));
            tx.commit();
        }
    } catch (...) {
        db.exec("PRAGMA foreign_keys = ON");
        throw;
    }
    db.exec("PRAGMA foreign_keys = ON");
    return target;
}
