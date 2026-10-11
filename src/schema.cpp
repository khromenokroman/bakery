#include "schema.hpp"

#include <array>
#include <string>
#include <string_view>

namespace {

/// Миграции схемы. Применённую миграцию не менять: изменения только новой миграцией в конце.
constexpr std::array<std::string_view, 2> MIGRATIONS = {
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
};

} // namespace

int schema_version() { return static_cast<int>(MIGRATIONS.size()); }

int current_schema_version(Database &db) {
    auto stmt = db.prepare("PRAGMA user_version");
    stmt.step();
    return static_cast<int>(stmt.column_int(0));
}

int migrate(Database &db) {
    auto const current = current_schema_version(db);
    if (current > schema_version()) {
        throw DbError("база создана более новой версией программы: схема " + std::to_string(current) + ", поддерживается " +
                      std::to_string(schema_version()));
    }
    for (auto v = current; v < schema_version(); ++v) {
        Transaction tx{db};
        db.exec(MIGRATIONS[static_cast<std::size_t>(v)]);
        db.exec("PRAGMA user_version = " + std::to_string(v + 1));
        tx.commit();
    }
    return schema_version();
}
