#include "report.hpp"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "errors.hpp"
#include "fixture.hpp"
#include "groups.hpp"
#include "prices.hpp"
#include "test_util.hpp"

using testing::StrEq;

namespace {

/**
 * @brief Себестоимость батона 31,20, торта нет. 10.10 поступило 40 батонов и 3 кг торта; 12.10 продано
 * 30 батонов; 13.10 возвращено 5; 14.10 списано 2. Отчёт за 12-14.10.
 */
class ReportTest : public DbFixture {
   protected:
    ReportTest() : m_bread{add("Батон", Kind::PRODUCT, Unit::PCS)}, m_cake{add("Торт", Kind::PRODUCT, Unit::KG)} {
        Prices prices{m_db};
        auto const v1 = prices.create_type("Опт");
        Document cost;
        cost.type = DocType::PRICES;
        cost.date = "2026-10-01";
        cost.price_type_id = COST_PRICE_TYPE;
        DocLine l;
        l.item_id = m_bread;
        l.price_kop = 3120;
        cost.lines = {l};
        m_docs.post(m_docs.create(cost));

        m_docs.post(m_docs.create(make_doc(DocType::PRODUCTION, "2026-10-10", {{m_bread, 40000}, {m_cake, 3000}})));
        auto sale = make_doc(DocType::SALE, "2026-10-12", {{m_bread, 30000}});
        sale.price_type_id = v1;
        sale.lines[0].price_kop = 4500;
        m_sale = m_docs.create(sale);
        m_docs.post(m_sale);
        auto ret = make_doc(DocType::RETURN, "2026-10-13", {{m_bread, 5000}});
        ret.base_id = m_sale;
        m_docs.post(m_docs.create(ret));
        m_docs.post(m_docs.create(make_doc(DocType::WRITEOFF, "2026-10-14", {{m_bread, 2000}}, "брак")));
    }

    std::int64_t m_bread;
    std::int64_t m_cake;
    std::int64_t m_sale{};
};

} // namespace

/**
 * @brief Остаток на начало, приход, расход и остаток на конец в суммах по себестоимости сходятся.
 *
 * Начало 40 × 31,20 = 1 248,00 (торт без себестоимости - 0); приход: возврат 5 × 31,20 = 156,00;
 * расход: реализация 30 × 31,20 = 936,00, списание 2 × 31,20 = 62,40; конец 1 248 + 156 - 998,40 = 405,60.
 */
TEST_F(ReportTest, PeriodSums) {
    auto const r = goods_report(m_db, "2026-10-12", "2026-10-14");
    EXPECT_EQ(r.opening_sum, 124800);
    EXPECT_EQ(r.in_sum, 15600);
    EXPECT_EQ(r.out_sum, 99840);
    EXPECT_EQ(r.closing_sum, 40560);
    ASSERT_EQ(r.incoming.size(), 1U);
    EXPECT_EQ(r.incoming[0].type, DocType::RETURN);
    ASSERT_EQ(r.outgoing.size(), 2U);
    EXPECT_EQ(r.outgoing[0].type, DocType::SALE);
    EXPECT_EQ(r.outgoing[0].sum, 93600);
    EXPECT_EQ(r.outgoing[1].type, DocType::WRITEOFF);
    EXPECT_EQ(r.lines_without_cost, 1) << "строка торта в поступлении без себестоимости";
}

/**
 * @brief Количество по позициям: начало, приход, расход, конец.
 */
TEST_F(ReportTest, Items) {
    auto const r = goods_report(m_db, "2026-10-12", "2026-10-14");
    ASSERT_EQ(r.items.size(), 2U);
    auto const &bread = r.items[0];
    EXPECT_THAT(bread.name, StrEq("Батон"));
    EXPECT_EQ(bread.opening, 40000);
    EXPECT_EQ(bread.in, 5000);
    EXPECT_EQ(bread.out, 32000);
    EXPECT_EQ(bread.closing, 13000);
    EXPECT_EQ(bread.closing_sum, 40560);
    EXPECT_EQ(r.items[1].opening, 3000) << "торт без движений в периоде, но с остатком";
    EXPECT_EQ(r.items[1].closing, 3000);
}

/**
 * @brief Поступление 10-го попадает в отчёт за 10-е как приход; отчёт за период без движений пустой по документам.
 */
TEST_F(ReportTest, PeriodBoundaries) {
    auto const r = goods_report(m_db, "2026-10-10", "2026-10-10");
    EXPECT_EQ(r.opening_sum, 0);
    ASSERT_EQ(r.incoming.size(), 1U);
    EXPECT_EQ(r.incoming[0].sum, 124800);
    auto const later = goods_report(m_db, "2026-10-20", "2026-10-21");
    EXPECT_TRUE(later.incoming.empty() && later.outgoing.empty());
    EXPECT_EQ(later.opening_sum, later.closing_sum);
    EXPECT_THROW_REASON((void)goods_report(m_db, "2026-10-14", "2026-10-12"), UserError, "начало периода позже конца");
}

/**
 * @brief Фильтр по группе оставляет только её позиции и документы с ними.
 */
TEST_F(ReportTest, Group) {
    Groups groups{m_db};
    Group g;
    g.name = "Торты";
    auto const cakes = groups.create(g);
    auto cake = m_catalog.get(m_cake);
    cake.group_id = cakes;
    m_catalog.update(cake);
    auto const r = goods_report(m_db, "2026-10-12", "2026-10-14", cakes);
    ASSERT_EQ(r.items.size(), 1U);
    EXPECT_THAT(r.items[0].name, StrEq("Торт"));
    EXPECT_TRUE(r.outgoing.empty()) << "реализация и списание были только по батону";
}
