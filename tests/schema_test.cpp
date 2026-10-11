#include "schema.hpp"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "db.hpp"

using testing::HasSubstr;

namespace {

std::vector<std::string> table_names(Database &db) {
    std::vector<std::string> names;
    auto stmt = db.prepare("SELECT name FROM sqlite_master WHERE type = 'table' ORDER BY name");
    while (stmt.step()) {
        names.push_back(stmt.column_text(0));
    }
    return names;
}

} // namespace

/**
 * @brief Пустая база получает все таблицы и последнюю версию схемы.
 */
TEST(Schema, MigratesEmptyDatabase) {
    Database db{":memory:"};
    ASSERT_EQ(current_schema_version(db), 0) << "новая база должна быть пустой";
    EXPECT_EQ(migrate(db), schema_version());
    EXPECT_EQ(current_schema_version(db), schema_version()) << "user_version после миграции";
    EXPECT_THAT(table_names(db), testing::IsSupersetOf({"items", "recipes", "recipe_lines", "price_types", "groups", "partners", "users", "docs",
                                                        "doc_lines", "stock_moves"}));
}

/**
 * @brief Повторная миграция ничего не меняет и не падает на уже созданных таблицах.
 */
TEST(Schema, MigrateIsIdempotent) {
    Database db{":memory:"};
    migrate(db);
    EXPECT_NO_THROW(migrate(db)) << "повторный запуск на актуальной базе";
    EXPECT_EQ(current_schema_version(db), schema_version());
}

/**
 * @brief База от более новой версии программы не открывается, чтобы не испортить данные.
 */
TEST(Schema, RejectsNewerDatabase) {
    Database db{":memory:"};
    db.exec("PRAGMA user_version = " + std::to_string(schema_version() + 1));
    try {
        migrate(db);
        FAIL() << "ожидался отказ для схемы " << schema_version() + 1;
    } catch (DbError const &e) {
        EXPECT_THAT(std::string(e.what()), HasSubstr("более новой версией программы"));
    }
}

/**
 * @brief Внешние ключи включены: строка рецептуры на несуществующий товар отклоняется.
 */
TEST(Schema, ForeignKeysEnabled) {
    Database db{":memory:"};
    migrate(db);
    try {
        db.exec("INSERT INTO recipes (item_id, output_qty) VALUES (42, 1000)");
        FAIL() << "рецептура для несуществующего товара 42 не должна вставляться";
    } catch (DbError const &e) {
        EXPECT_THAT(std::string(e.what()), HasSubstr("FOREIGN KEY"));
    }
}

/**
 * @brief Транзакция без commit() откатывается в деструкторе.
 */
TEST(Schema, TransactionRollsBack) {
    Database db{":memory:"};
    migrate(db);
    {
        Transaction tx{db};
        db.exec("INSERT INTO items (name, kind, unit) VALUES ('Мука', 'raw', 'kg')");
    }
    auto stmt = db.prepare("SELECT count(*) FROM items");
    stmt.step();
    EXPECT_EQ(stmt.column_int(0), 0) << "вставка вне закоммиченной транзакции должна быть отменена";
}

/**
 * @brief Миграция 3 пересобирает таблицу документов: документы, строки и движения сохраняются,
 * ссылки строк на документы работают, внешние ключи после миграции снова включены.
 */
TEST(Schema, Migration3KeepsDocuments) {
    Database db{":memory:"};
    ASSERT_EQ(migrate(db, 2), 2);
    db.exec(
        "INSERT INTO items (id, name, kind, unit) VALUES (1, 'Батон', 'product', 'pcs');"
        "INSERT INTO docs (id, type, number, date, status) VALUES (7, 'production', 1, '2026-10-12', 'posted');"
        "INSERT INTO doc_lines (doc_id, item_id, qty) VALUES (7, 1, 40000);"
        "INSERT INTO stock_moves (doc_id, item_id, qty, date) VALUES (7, 1, 40000, '2026-10-12');");
    ASSERT_EQ(migrate(db), schema_version());

    auto stmt = db.prepare(
        "SELECT d.type, d.status, l.qty, l.cost_kop IS NULL, m.qty FROM docs d JOIN doc_lines l ON l.doc_id = d.id "
        "JOIN stock_moves m ON m.doc_id = d.id WHERE d.id = 7");
    ASSERT_TRUE(stmt.step()) << "документ 7 со строкой и движением должен сохраниться";
    EXPECT_EQ(stmt.column_text(0), "production");
    EXPECT_EQ(stmt.column_text(1), "posted");
    EXPECT_EQ(stmt.column_int(2), 40000);
    EXPECT_EQ(stmt.column_int(3), 1) << "себестоимость у старых строк пустая";
    EXPECT_EQ(stmt.column_int(4), 40000);

    db.exec("DELETE FROM docs WHERE id = 7");
    auto lines = db.prepare("SELECT count(*) FROM doc_lines");
    lines.step();
    EXPECT_EQ(lines.column_int(0), 0) << "каскадное удаление строк должно работать после пересборки docs";
    try {
        db.exec("INSERT INTO doc_lines (doc_id, item_id, qty) VALUES (999, 1, 1)");
        FAIL() << "внешние ключи после миграции должны быть включены";
    } catch (DbError const &e) {
        EXPECT_THAT(std::string(e.what()), HasSubstr("FOREIGN KEY"));
    }
}
