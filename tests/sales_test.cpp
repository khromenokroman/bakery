#include <gtest/gtest.h>

#include <optional>
#include <string>

#include "errors.hpp"
#include "fixture.hpp"
#include "groups.hpp"
#include "prices.hpp"
#include "report.hpp"
#include "test_util.hpp"

namespace {

/**
 * @brief Продажи за период: реализация минус возврат, разбивка по группе, где числится товар.
 *
 * Батон («Батоны»/«Хлеб»): себестоимость 31,20, продано 30 по 45,00, возвращено 5.
 * Торт («Кондитерка», кг): себестоимость 100,00, продано 5 по 150,00.
 * Круассан (без группы): себестоимость 10,00, продано 2 по 20,00.
 */
class SalesTest : public DbFixture {
   protected:
    SalesTest() {
        m_loaves = group("Батоны", group("Хлеб"));
        m_pastry = group("Кондитерка");
        m_bread = item("Батон", m_loaves);
        m_cake = item("Торт", m_pastry, Unit::KG);
        m_croissant = item("Круассан", std::nullopt);
        cost(m_bread, 3120, "2026-10-10");
        cost(m_cake, 10000, "2026-10-10");
        cost(m_croissant, 1000, "2026-10-10");
        m_v1 = m_prices.create_type("Вариант 1");
        m_docs.post(m_docs.create(make_doc(DocType::PRODUCTION, "2026-10-11", {{m_bread, 40000}, {m_cake, 6000}, {m_croissant, 10000}})));
        auto sale = make_doc(DocType::SALE, "2026-10-12", {{m_bread, 30000}, {m_cake, 5000}, {m_croissant, 2000}});
        sale.price_type_id = m_v1;
        sale.lines[0].price_kop = 4500;
        sale.lines[1].price_kop = 15000;
        sale.lines[2].price_kop = 2000;
        m_sale = m_docs.create(sale);
        m_docs.post(m_sale);
    }

    std::int64_t group(std::string const &name, std::optional<std::int64_t> parent = std::nullopt) {
        Group g;
        g.name = name;
        g.parent_id = parent;
        return m_groups.create(g);
    }

    std::int64_t item(std::string const &name, std::optional<std::int64_t> grp, Unit unit = Unit::PCS) {
        Item it;
        it.name = name;
        it.kind = Kind::PRODUCT;
        it.unit = unit;
        it.group_id = grp;
        return m_catalog.create(it);
    }

    void cost(std::int64_t itm, std::int64_t price, std::string const &date) {
        Document d;
        d.type = DocType::PRICES;
        d.date = date;
        d.price_type_id = COST_PRICE_TYPE;
        DocLine l;
        l.item_id = itm;
        l.price_kop = price;
        d.lines = {l};
        m_docs.post(m_docs.create(d));
    }

    Groups m_groups{m_db};
    Prices m_prices{m_db};
    std::int64_t m_v1{};
    std::int64_t m_bread{};
    std::int64_t m_cake{};
    std::int64_t m_croissant{};
    std::int64_t m_loaves{};
    std::int64_t m_pastry{};
    std::int64_t m_sale{};
};

SalesGroup const *find(SalesReport const &r, std::int64_t id) {
    for (auto const &g : r.groups) {
        if (g.group_id == id) {
            return &g;
        }
    }
    return nullptr;
}

} // namespace

/**
 * @brief Нетто, группы, итоги, итоговая наценка.
 */
TEST_F(SalesTest, NetTotalsByGroup) {
    auto ret = make_doc(DocType::RETURN, "2026-10-13", {{m_bread, 5000}});
    ret.base_id = m_sale;
    m_docs.post(m_docs.create(ret));

    auto const r = sales_report(m_db, "2026-10-01", "2026-12-31");
    ASSERT_EQ(r.groups.size(), 3) << "Батоны, Кондитерка, Без группы";
    auto *bread = find(r, m_loaves);
    auto *cake = find(r, m_pastry);
    auto *noGroup = find(r, 0);
    ASSERT_NE(bread, nullptr);
    ASSERT_NE(cake, nullptr);
    ASSERT_NE(noGroup, nullptr);

    ASSERT_EQ(bread->items.size(), 1);
    EXPECT_EQ(bread->items[0].qty, 25000) << "30 - 5, нетто";
    EXPECT_EQ(bread->items[0].cost, 78000) << "25 × 31,20 = 780,00";
    EXPECT_EQ(bread->items[0].revenue, 112500) << "25 × 45,00 = 1125,00";
    EXPECT_EQ(bread->items[0].margin, 34500);
    EXPECT_EQ(bread->items[0].margin_pct, 44) << "34500*100/78000 = 44";
    EXPECT_EQ(bread->cost, 78000);
    EXPECT_EQ(bread->revenue, 112500);
    EXPECT_EQ(bread->margin, 34500);
    EXPECT_EQ(bread->margin_pct, 44) << "34500*100/78000 = 44";

    ASSERT_EQ(cake->items.size(), 1);
    EXPECT_EQ(cake->items[0].qty, 5000);
    EXPECT_EQ(cake->items[0].cost, 50000) << "5 кг × 100,00 = 500,00";
    EXPECT_EQ(cake->items[0].revenue, 75000) << "5 кг × 150,00 = 750,00";
    EXPECT_EQ(cake->items[0].margin, 25000);
    EXPECT_EQ(cake->margin_pct, 50);

    ASSERT_EQ(noGroup->items.size(), 1);
    EXPECT_EQ(noGroup->name, "Без группы");
    EXPECT_EQ(noGroup->items[0].qty, 2000);
    EXPECT_EQ(noGroup->items[0].cost, 2000);
    EXPECT_EQ(noGroup->items[0].revenue, 4000);
    EXPECT_EQ(noGroup->items[0].margin, 2000);
    EXPECT_EQ(noGroup->margin_pct, 100) << "2000*100/2000 = 100";

    EXPECT_EQ(r.total_cost, 78000 + 50000 + 2000);
    EXPECT_EQ(r.total_revenue, 112500 + 75000 + 4000);
    EXPECT_EQ(r.total_margin, r.total_revenue - r.total_cost);
    EXPECT_EQ(r.total_margin_pct, (r.total_margin * 100 + r.total_cost / 2) / r.total_cost);
}

/**
 * @brief Фильтр по верхней группе: подгруппы включаются, чужие - нет.
 */
TEST_F(SalesTest, GroupFilter) {
    std::optional<std::int64_t> khleb;
    for (auto const &g : m_groups.list()) {
        if (g.name == "Хлеб") {
            khleb = g.id;
        }
    }
    ASSERT_TRUE(khleb.has_value());
    auto const r = sales_report(m_db, "2026-10-01", "2026-12-31", khleb);
    ASSERT_EQ(r.groups.size(), 1) << "только «Батоны» (батон): торт и круассан вне «Хлеб»";
    EXPECT_EQ(r.total_qty, 30000) << "батон продан 30 (возврата в этом тесте нет)";
    auto const all = sales_report(m_db, "2026-10-01", "2026-12-31");
    EXPECT_EQ(all.groups.size(), 3);
    EXPECT_EQ(all.total_qty, 37000) << "30 (батон) + 5 (торт) + 2 (круассан)";
}

/**
 * @brief Пустой период - пустой отчёт; неправильные даты - ошибка.
 */
TEST_F(SalesTest, EmptyAndInvalid) {
    EXPECT_THROW_REASON(sales_report(m_db, "2026-11-02", "2026-11-01"), UserError, "начало периода позже конца");
    auto const r = sales_report(m_db, "2027-01-01", "2027-01-31");
    EXPECT_TRUE(r.groups.empty());
    EXPECT_EQ(r.total_cost, 0);
    EXPECT_EQ(r.total_revenue, 0);
    EXPECT_EQ(r.total_margin, 0);
    EXPECT_EQ(r.total_margin_pct, 0);
}

/**
 * @brief Отчёт за период с продажами: итог по всем трём группам (батон, торт, круассан).
 */
TEST_F(SalesTest, TotalsAllThreeGroups) {
    auto const r = sales_report(m_db, "2026-10-01", "2026-10-31");
    ASSERT_EQ(r.groups.size(), 3);
    EXPECT_EQ(r.total_qty, 37000);
    // себестоимость: (30000*3120 + 5000*10000 + 2000*1000) / 1000 = to_kop
    EXPECT_EQ(r.total_cost, 145600);
    // выручка: (30000*4500 + 5000*15000 + 2000*2000) / 1000 = to_kop
    EXPECT_EQ(r.total_revenue, 214000);
    EXPECT_EQ(r.total_margin, r.total_revenue - r.total_cost);
}
