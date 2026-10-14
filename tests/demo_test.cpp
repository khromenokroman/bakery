#include "demo.hpp"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "errors.hpp"
#include "report.hpp"
#include "repost.hpp"
#include "schema.hpp"
#include "stock.hpp"
#include "test_util.hpp"
#include "users.hpp"

namespace {

/// Количество строк запроса count(*).
std::int64_t count(Database &db, std::string const &sql) {
    auto stmt = db.prepare(sql);
    stmt.step();
    return stmt.column_int(0);
}

} // namespace

/**
 * @brief Демонстрационная база за год: объёмы справочников и документов, остатки не в минусе,
 * перепроведение не нужно, товарный отчёт за год сходится, пользователи входят с паролем demo123.
 */
TEST(Demo, FillsYear) {
    Database db{":memory:"};
    migrate(db);
    auto const stats = fill_demo(db, "2026-10-14");
    EXPECT_TRUE(is_demo(db));
    EXPECT_GE(stats.items, 300) << "позиций номенклатуры";
    EXPECT_LE(stats.items, 400);
    EXPECT_EQ(stats.partners, 100) << "клиентов";
    EXPECT_EQ(count(db, "SELECT count(*) FROM partners"), 100);
    EXPECT_EQ(count(db, "SELECT count(*) FROM docs"), stats.docs);
    auto const days = count(db, "SELECT count(DISTINCT substr(date, 1, 10)) FROM docs WHERE type = 'sale'");
    EXPECT_GE(days, 365) << "реализации каждый день года";
    EXPECT_GE(stats.docs / 365, 90) << "около 100 документов в день, всего " << stats.docs;
    EXPECT_GT(count(db, "SELECT count(*) FROM docs WHERE type = 'return'"), 0);
    EXPECT_GT(count(db, "SELECT count(*) FROM docs WHERE type = 'writeoff'"), 0);
    EXPECT_GE(count(db, "SELECT count(*) FROM docs WHERE type = 'inventory'"), 12) << "инвентаризация раз в месяц";
    EXPECT_GE(count(db, "SELECT count(*) FROM docs WHERE type = 'prices'"), 3 * 4) << "начальные цены и три переоценки, по три типа";
    EXPECT_EQ(count(db, "SELECT count(*) FROM docs WHERE type = 'sale' AND status = 'draft'"), 1) << "черновик сегодня";
    EXPECT_GE(count(db, "SELECT count(*) FROM docs WHERE type = 'order' AND date >= '2026-10-15'"), 1) << "заявка на завтра";
    EXPECT_EQ(count(db, "SELECT count(*) FROM docs WHERE type = 'sale' AND partner_id IS NULL"), 0) << "у реализаций есть клиент";
    EXPECT_GT(count(db, "SELECT count(*) FROM docs WHERE created_by IS NOT NULL"), 0) << "у документов есть авторы";

    EXPECT_FALSE(Reposter{db}.state().from) << "документы созданы по порядку времени";
    for (auto const &b : Stock{db}.balances(std::nullopt)) {
        EXPECT_GE(b.qty, 0) << b.name;
    }
    auto const report = goods_report(db, "2025-10-15", "2026-10-14", std::nullopt, std::nullopt);
    EXPECT_EQ(report.closing_sum, report.opening_sum + report.in_sum - report.out_sum);
    EXPECT_EQ(report.lines_without_cost, 0) << "себестоимость у всех движений";

    Users users{db};
    for (auto const *login : {"admin", "tech", "seller"}) {
        EXPECT_NO_THROW((void)users.login(login, DEMO_PASSWORD)) << login;
    }
    EXPECT_THROW_REASON(fill_demo(db, "2026-10-14"), UserError, "только в пустой базе");
}
