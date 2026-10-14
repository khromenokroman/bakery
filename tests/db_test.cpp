#include "db.hpp"

#include <gtest/gtest.h>

namespace {

std::int64_t rows(Database &db) {
    auto stmt = db.prepare("SELECT count(*) FROM t");
    stmt.step();
    return stmt.column_int(0);
}

} // namespace

/**
 * @brief Вложенная транзакция - точка сохранения: её откат убирает только её изменения, фиксирует внешняя.
 */
TEST(Db, NestedTransaction) {
    Database db{":memory:"};
    db.exec("CREATE TABLE t (x INTEGER)");
    {
        Transaction outer{db};
        db.exec("INSERT INTO t VALUES (1)");
        {
            Transaction inner{db};
            db.exec("INSERT INTO t VALUES (2)");
        }
        EXPECT_EQ(rows(db), 1) << "откат вложенной убирает только её строку";
        {
            Transaction inner{db};
            db.exec("INSERT INTO t VALUES (3)");
            inner.commit();
        }
        EXPECT_TRUE(db.in_transaction()) << "внешняя ещё идёт";
        outer.commit();
    }
    EXPECT_FALSE(db.in_transaction());
    EXPECT_EQ(rows(db), 2) << "внешняя зафиксировала свою строку и вложенную";
    {
        Transaction outer{db};
        Transaction inner{db};
        db.exec("INSERT INTO t VALUES (4)");
        inner.commit();
    }
    EXPECT_EQ(rows(db), 2) << "откат внешней убирает и отпущенную вложенную";
}

/**
 * @brief Один и тот же запрос во вложенных вызовах: кеш выдаёт отдельный экземпляр, итерации не мешают
 * друг другу; возвращённый в кеш запрос сброшен и без параметров.
 */
TEST(Db, StatementCacheNested) {
    Database db{":memory:"};
    db.exec("CREATE TABLE t (x INTEGER); INSERT INTO t VALUES (1), (2), (3)");
    constexpr char const sql[] = "SELECT x FROM t WHERE x >= ? ORDER BY x";
    auto outer = db.prepare(sql);
    outer.bind(1, 1);
    std::int64_t pairs = 0;
    while (outer.step()) {
        auto inner = db.prepare(sql);
        inner.bind(1, outer.column_int(0));
        while (inner.step()) {
            ++pairs;
        }
    }
    EXPECT_EQ(pairs, 3 + 2 + 1) << "вложенный запрос с тем же текстом не сбивает внешний";
    {
        auto again = db.prepare(sql);
        EXPECT_FALSE(again.step()) << "из кеша - без старого параметра (NULL): строк нет";
    }
}
