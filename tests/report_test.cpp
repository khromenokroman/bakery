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

/**
 * @brief Вторая оценка по типу цен: каждое движение по цене на момент документа.
 *
 * «Розница»: батон 45,00 с 01.10, 50,00 с 13.10; торта нет. Начало (до 12.10): 40 × 45 = 1 800,00;
 * приход: возврат 13.10 5 × 50 = 250,00; расход: реализация 12.10 30 × 45 = 1 350,00, списание
 * 14.10 2 × 50 = 100,00; конец 1 800 + 250 - 1 450 = 600,00.
 */
TEST_F(ReportTest, PriceTypeColumn) {
    Prices prices{m_db};
    auto const retail = prices.create_type("Розница");
    for (auto const &[date, price] : {std::pair{"2026-10-01", 4500}, std::pair{"2026-10-13", 5000}}) {
        Document d;
        d.type = DocType::PRICES;
        d.date = date;
        d.price_type_id = retail;
        DocLine l;
        l.item_id = m_bread;
        l.price_kop = price;
        d.lines = {l};
        m_docs.post(m_docs.create(d));
    }
    auto const r = goods_report(m_db, "2026-10-12", "2026-10-14", std::nullopt, retail);
    EXPECT_EQ(r.opening_price_sum, 180000);
    EXPECT_EQ(r.in_price_sum, 25000);
    EXPECT_EQ(r.out_price_sum, 145000);
    EXPECT_EQ(r.closing_price_sum, 60000);
    ASSERT_EQ(r.outgoing.size(), 2U);
    EXPECT_EQ(r.outgoing[0].price_sum, 135000) << "реализация 12.10 по 45,00";
    EXPECT_EQ(r.outgoing[1].price_sum, 10000) << "списание 14.10 по 50,00";
    EXPECT_EQ(r.moves_without_price, 1) << "поступление торта: торта нет в «Рознице»";
    EXPECT_EQ(r.closing_sum, 40560) << "себестоимость считается как прежде";

    auto const plain = goods_report(m_db, "2026-10-12", "2026-10-14");
    EXPECT_FALSE(plain.price_type_id);
    EXPECT_EQ(plain.closing_price_sum, 0) << "без типа цен вторая оценка не считается";
}

/**
 * @brief Остаток на начало по типу цен складывается из интервалов действия цен до периода.
 *
 * «Розница»: 45,00 с 01.10, 50,00 с 11.10. До периода 12.10: поступление 10.10 40 × 45 = 1 800,00 и
 * списание 11.10 1 × 50 = 50,00; начало = 1 750,00 (не 39 × текущую цену).
 */
TEST_F(ReportTest, OpeningByPriceIntervals) {
    m_docs.post(m_docs.create(make_doc(DocType::WRITEOFF, "2026-10-11", {{m_bread, 1000}}, "брак")));
    Prices prices{m_db};
    auto const retail = prices.create_type("Розница");
    for (auto const &[date, price] : {std::pair{"2026-10-01", 4500}, std::pair{"2026-10-11", 5000}}) {
        Document d;
        d.type = DocType::PRICES;
        d.date = date;
        d.price_type_id = retail;
        DocLine l;
        l.item_id = m_bread;
        l.price_kop = price;
        d.lines = {l};
        m_docs.post(m_docs.create(d));
    }
    auto const r = goods_report(m_db, "2026-10-12", "2026-10-14", std::nullopt, retail);
    EXPECT_EQ(r.opening_price_sum, 175000);
    EXPECT_EQ(r.closing_price_sum, r.opening_price_sum + r.in_price_sum - r.out_price_sum);
}
