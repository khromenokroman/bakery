#include "stock.hpp"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <sstream>

#include "errors.hpp"
#include "fixture.hpp"
#include "test_util.hpp"

using testing::StrEq;

namespace {

/**
 * @brief Склад с батоном (шт) и пирогом (кг).
 */
class StockTest : public DbFixture {
   protected:
    StockTest() : m_stock{m_db}, m_bread{add("Батон", Kind::PRODUCT, Unit::PCS)}, m_pie{add("Пирог", Kind::PRODUCT, Unit::KG)} {}

    std::int64_t production(std::string const &date, std::vector<std::pair<std::int64_t, std::int64_t>> const &lines) {
        return m_docs.create(make_doc(DocType::PRODUCTION, date, lines));
    }

    /// Остатки строкой "Название=кол-во; " на конец дня date (все, если пусто).
    std::string balances(std::optional<std::string> const &date = std::nullopt) const {
        std::ostringstream out;
        for (auto const &b : m_stock.balances(date)) {
            out << b.name << "=" << b.qty << "; ";
        }
        return out.str();
    }

    /// Имитация будущей реализации: движение со знаком минус, записанное напрямую.
    void fake_sale(std::string const &date, std::int64_t item, std::int64_t qty) {
        auto const doc = m_docs.create(make_order(date, {{item, qty}}));
        auto stmt = m_db.prepare("INSERT INTO stock_moves (doc_id, item_id, qty, date) VALUES (?, ?, ?, ?)");
        stmt.bind(1, doc).bind(2, item).bind(3, -qty).bind(4, date);
        stmt.step();
    }

    Stock m_stock;
    std::int64_t m_bread;
    std::int64_t m_pie;
};

} // namespace

/**
 * @brief Количество с единицей для сообщений: запятая, без лишних нулей, знак минус.
 */
TEST(FormatQty, Cases) {
    EXPECT_THAT(format_qty(1500, Unit::KG), StrEq("1,5 кг"));
    EXPECT_THAT(format_qty(8000, Unit::PCS), StrEq("8 шт"));
    EXPECT_THAT(format_qty(1, Unit::L), StrEq("0,001 л"));
    EXPECT_THAT(format_qty(20250, Unit::KG), StrEq("20,25 кг"));
    EXPECT_THAT(format_qty(-500, Unit::KG), StrEq("-0,5 кг"));
}

/**
 * @brief Проведённое поступление даёт остаток, остаток на дату учитывает только движения до её конца.
 */
TEST_F(StockTest, PostedProductionMakesBalance) {
    auto const a = production("2026-10-12", {{m_bread, 40000}, {m_pie, 2500}});
    auto const b = production("2026-10-13", {{m_bread, 5000}});
    EXPECT_THAT(balances(), StrEq("")) << "непроведённые документы остатков не дают";
    m_docs.post(a);
    m_docs.post(b);
    EXPECT_EQ(m_docs.get(a).status, "posted");
    EXPECT_THAT(balances(), StrEq("Батон=45000; Пирог=2500; "));
    EXPECT_THAT(balances("2026-10-12"), StrEq("Батон=40000; Пирог=2500; ")) << "на конец 12-го";
    EXPECT_THAT(balances("2026-10-11"), StrEq("")) << "до поступлений";
}

/**
 * @brief Отмена проведения убирает движения; повторное проведение и отмена непроведённого отклоняются.
 */
TEST_F(StockTest, PostAndUnpost) {
    auto const id = production("2026-10-12", {{m_bread, 10000}});
    EXPECT_THROW_REASON(m_docs.unpost(id), UserError, "документ не проведён");
    m_docs.post(id);
    EXPECT_THROW_REASON(m_docs.post(id), UserError, "документ уже проведён");
    m_docs.unpost(id);
    EXPECT_EQ(m_docs.get(id).status, "draft");
    EXPECT_THAT(balances(), StrEq("")) << "после отмены проведения";
}

/**
 * @brief Проведённый документ нельзя изменить или удалить, заявку нельзя провести.
 */
TEST_F(StockTest, PostedIsReadOnly) {
    auto const id = production("2026-10-12", {{m_bread, 10000}});
    m_docs.post(id);
    auto doc = make_doc(DocType::PRODUCTION, "2026-10-12", {{m_bread, 1000}});
    doc.id = id;
    EXPECT_THROW_REASON(m_docs.update(doc), UserError, "документ проведён, изменить его нельзя");
    EXPECT_THROW_REASON(m_docs.remove(id), UserError, "документ проведён, удалить его нельзя");
    EXPECT_THROW_REASON(m_catalog.remove(m_bread), UserError, "есть в документах");

    auto const order = m_docs.create(make_order("2026-10-12", {{m_bread, 1000}}));
    EXPECT_THROW_REASON(m_docs.post(order), UserError, "документ этого вида не проводится");
}

/**
 * @brief Отмену поступления, из которого уже продано, запрещает проверка минуса; документ остаётся проведённым.
 */
TEST_F(StockTest, UnpostBlockedBySale) {
    auto const id = production("2026-10-12", {{m_bread, 10000}});
    m_docs.post(id);
    fake_sale("2026-10-13", m_bread, 8000);
    EXPECT_THROW_REASON(m_docs.unpost(id), UserError, "«Батон»: остаток на 13.10.2026 уйдёт в минус, не хватает 8 шт");
    EXPECT_EQ(m_docs.get(id).status, "posted") << "после отказа транзакция должна откатиться";
    EXPECT_THAT(balances(), StrEq("Батон=2000; "));
}

/**
 * @brief Минус проверяется на конец каждого дня, а не по итогу: продажа 11-го при поступлении 12-го не проходит.
 */
TEST_F(StockTest, NegativeOnEarlierDay) {
    auto const early = production("2026-10-11", {{m_bread, 5000}});
    m_docs.post(early);
    fake_sale("2026-10-11", m_bread, 5000);
    auto const later = production("2026-10-12", {{m_bread, 5000}});
    m_docs.post(later);
    EXPECT_THAT(balances(), StrEq("Батон=5000; "));
    // итог без early был бы 0, но 11-го продано 5 шт, которых без него не было
    EXPECT_THROW_REASON(m_docs.unpost(early), UserError, "остаток на 11.10.2026 уйдёт в минус, не хватает 5 шт");
}

/**
 * @brief Поступление и продажа в один день: важен итог дня, а не порядок документов внутри него.
 */
TEST_F(StockTest, SameDayIsFine) {
    fake_sale("2026-10-12", m_pie, 1000);
    auto const id = production("2026-10-12", {{m_pie, 1000}});
    EXPECT_NO_THROW(m_docs.post(id)) << "продажа и поступление 12-го, итог дня 0";
}

/**
 * @brief В поступление входит только продукция; список фильтруется по нескольким видам.
 */
TEST_F(StockTest, ProductionValidationAndListTypes) {
    auto const dough = add("Тесто", Kind::SEMI);
    EXPECT_THROW_REASON(production("2026-10-12", {{dough, 1000}}), UserError, "в документ входит только продукция");

    production("2026-10-12", {{m_bread, 1000}});
    m_docs.create(make_order("2026-10-12", {{m_bread, 1000}}));
    DocFilter filter;
    filter.types = {DocType::PRODUCTION};
    EXPECT_EQ(m_docs.list(filter).size(), 1U) << "только поступления";
    filter.types = {DocType::PRODUCTION, DocType::ORDER};
    EXPECT_EQ(m_docs.list(filter).size(), 2U) << "оба вида";
    EXPECT_EQ(m_docs.get(m_docs.list(filter)[0].id).number, 1) << "нумерация у каждого вида своя";
}

/**
 * @brief Документ задним числом сразу меняет остатки на все последующие даты: остатки не хранятся,
 * а считаются из движений при каждом запросе.
 */
TEST_F(StockTest, BackdatedDocumentRecalculates) {
    m_docs.post(production("2026-10-12", {{m_bread, 10000}}));
    EXPECT_THAT(balances("2026-10-11"), StrEq(""));
    m_docs.post(production("2026-10-10", {{m_bread, 3000}}));
    EXPECT_THAT(balances("2026-10-11"), StrEq("Батон=3000; ")) << "забытое поступление 10-го видно на 11-е";
    EXPECT_THAT(balances("2026-10-12"), StrEq("Батон=13000; ")) << "и на все даты после";
}
