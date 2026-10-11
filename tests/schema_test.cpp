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
    EXPECT_THAT(table_names(db),
                testing::IsSupersetOf({"items", "recipes", "recipe_lines", "prices", "partners", "users", "docs", "doc_lines", "stock_moves"}));
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
