#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "errors.hpp"
#include "fixture.hpp"
#include "partners.hpp"
#include "prices.hpp"
#include "stock.hpp"
#include "test_util.hpp"

using testing::StrEq;

namespace {

/**
 * @brief На складе 40 батонов и 5 кг торта (себестоимость батона 31,20); 12.10 «Колоску» продано
 * 30 батонов по 45,00 (реализация проведена).
 */
class ReturnsTest : public DbFixture {
   protected:
    ReturnsTest()
        : m_prices{m_db},
          m_bread{add("Батон", Kind::PRODUCT, Unit::PCS)},
          m_cake{add("Торт", Kind::PRODUCT, Unit::KG)},
          m_v1{m_prices.create_type("Вариант 1")} {
        Document cost;
        cost.type = DocType::PRICES;
        cost.date = "2026-10-10";
        cost.price_type_id = COST_PRICE_TYPE;
        DocLine l;
        l.item_id = m_bread;
        l.price_kop = 3120;
        cost.lines = {l};
        m_docs.post(m_docs.create(cost));
        m_docs.post(m_docs.create(make_doc(DocType::PRODUCTION, "2026-10-11", {{m_bread, 40000}, {m_cake, 5000}})));

        Partner p;
        p.name = "Колосок";
        m_shop = Partners{m_db}.create(p);
        auto sale = make_doc(DocType::SALE, "2026-10-12", {{m_bread, 30000}});
        sale.price_type_id = m_v1;
        sale.partner_id = m_shop;
        sale.lines[0].price_kop = 4500;
        m_sale = m_docs.create(sale);
        m_docs.post(m_sale);
    }

    Document return_doc(std::string const &date, std::vector<std::pair<std::int64_t, std::int64_t>> const &lines) const {
        auto d = make_doc(DocType::RETURN, date, lines);
        d.base_id = m_sale;
        return d;
    }

    std::int64_t balance(std::int64_t item) {
        for (auto const &b : Stock{m_db}.balances(std::nullopt)) {
            if (b.item_id == item) {
                return b.qty;
            }
        }
        return 0;
    }

    Prices m_prices;
    std::int64_t m_bread;
    std::int64_t m_cake;
    std::int64_t m_v1;
    std::int64_t m_shop{};
    std::int64_t m_sale{};
};

} // namespace

/**
 * @brief Возврат: клиент, тип цен, цена и себестоимость из реализации; проведение возвращает на склад.
 */
TEST_F(ReturnsTest, ReturnToStockWithSalePrices) {
    auto doc = return_doc("2026-10-13", {{m_bread, 5000}});
    doc.lines[0].price_kop = 1; // цену из запроса сервер не берёт
    auto const id = m_docs.create(doc);
    auto const saved = m_docs.get(id);
    EXPECT_EQ(saved.partner_id, m_shop) << "клиент из реализации";
    EXPECT_EQ(saved.price_type_id, m_v1);
    EXPECT_EQ(saved.lines[0].price_kop, 4500) << "цена из реализации";
    EXPECT_EQ(saved.lines[0].cost_kop, 3120) << "себестоимость из реализации";
    EXPECT_EQ(balance(m_bread), 10000);
    m_docs.post(id);
    EXPECT_EQ(balance(m_bread), 15000) << "10 + 5 возвращено";
}

/**
 * @brief Вернуть можно не больше проданного за вычетом проведённых возвратов; черновики не считаются.
 */
TEST_F(ReturnsTest, ReturnLimits) {
    EXPECT_THROW_REASON(m_docs.create(return_doc("2026-10-13", {{m_bread, 31000}})), UserError,
                        "«Батон»: можно вернуть не больше 30 шт (продано 30 шт, уже возвращено 0 шт)");
    auto const first = m_docs.create(return_doc("2026-10-13", {{m_bread, 20000}}));
    auto const second = m_docs.create(return_doc("2026-10-13", {{m_bread, 20000}}));
    m_docs.post(first);
    EXPECT_THROW_REASON(m_docs.post(second), UserError, "можно вернуть не больше 10 шт (продано 30 шт, уже возвращено 20 шт)");
    EXPECT_THROW_REASON(m_docs.create(return_doc("2026-10-13", {{m_bread, 11000}})), UserError, "не больше 10 шт");

    auto const r = m_docs.returnable(m_sale);
    ASSERT_EQ(r.size(), 1U);
    EXPECT_EQ(r[0].sold, 30000);
    EXPECT_EQ(r[0].returned, 20000);
    EXPECT_EQ(m_docs.returnable(m_sale, first)[0].returned, 0) << "редактируемый возврат исключается";

    auto edit = return_doc("2026-10-13", {{m_bread, 25000}});
    edit.id = first;
    EXPECT_NO_THROW(m_docs.update(edit)) << "перепроведение своего возврата не считает его самого";
}

/**
 * @brief Проверки основания: позиция из реализации, дата не раньше, реализация проведена.
 */
TEST_F(ReturnsTest, BaseChecks) {
    EXPECT_THROW_REASON(m_docs.create(return_doc("2026-10-13", {{m_cake, 1000}})), UserError, "«Торт» не было в: реализация № 1 от 12.10.2026");
    EXPECT_THROW_REASON(m_docs.create(return_doc("2026-10-11", {{m_bread, 1000}})), UserError, "возврат не может быть раньше реализации");
    auto no_base = make_doc(DocType::RETURN, "2026-10-13", {{m_bread, 1000}});
    EXPECT_THROW_REASON(m_docs.create(no_base), UserError, "не указана реализация");
    auto wrong = no_base;
    wrong.base_id = m_docs.create(make_order("2026-10-12", {{m_bread, 1000}}));
    EXPECT_THROW_REASON(m_docs.create(wrong), UserError, "основанием возврата может быть только реализация");

    auto draft_sale = make_doc(DocType::SALE, "2026-10-12", {{m_bread, 1000}});
    draft_sale.price_type_id = m_v1;
    auto unposted = no_base;
    unposted.base_id = m_docs.create(draft_sale);
    EXPECT_THROW_REASON(m_docs.create(unposted), UserError, "реализация № 2 от 12.10.2026 не проведена");
}

/**
 * @brief Реализацию с проведённым возвратом нельзя отменить, уменьшить ниже возвращённого или удалить.
 */
TEST_F(ReturnsTest, SaleProtectedByReturns) {
    auto const ret = m_docs.create(return_doc("2026-10-13", {{m_bread, 5000}}));
    m_docs.post(ret);
    EXPECT_THROW_REASON(m_docs.unpost(m_sale), UserError, "по реализации проведены возвраты № 1, сначала отмените их проведение");

    auto smaller = make_doc(DocType::SALE, "2026-10-12", {{m_bread, 3000}});
    smaller.id = m_sale;
    smaller.price_type_id = m_v1;
    smaller.lines[0].price_kop = 4500;
    EXPECT_THROW_REASON(m_docs.update(smaller), UserError, "«Батон»: по возвратам возвращено 5 шт, а в реализации останется 3 шт");
    smaller.lines[0].qty = 5000;
    EXPECT_NO_THROW(m_docs.update(smaller)) << "ровно возвращённое количество допустимо";

    m_docs.unpost(ret);
    EXPECT_NO_THROW(m_docs.unpost(m_sale)) << "после отмены возврата";
    EXPECT_THROW_REASON(m_docs.remove(m_sale), UserError, "по документу оформлены возвраты № 1, сначала удалите их");
    m_docs.remove(ret);
    EXPECT_NO_THROW(m_docs.remove(m_sale));
}

/**
 * @brief Списание: причина обязательна, себестоимость на дату, со склада с проверкой минуса.
 */
TEST_F(ReturnsTest, Writeoff) {
    auto doc = make_doc(DocType::WRITEOFF, "2026-10-13", {{m_bread, 3000}}, "  ");
    EXPECT_THROW_REASON(m_docs.create(doc), UserError, "укажите причину списания");
    doc.comment = "черствый, не продан";
    auto const id = m_docs.create(doc);
    EXPECT_EQ(m_docs.get(id).lines[0].cost_kop, 3120);
    m_docs.post(id);
    EXPECT_EQ(balance(m_bread), 7000) << "10 - 3";

    auto const too_much = m_docs.create(make_doc(DocType::WRITEOFF, "2026-10-13", {{m_cake, 6000}}, "брак"));
    EXPECT_THROW_REASON(m_docs.post(too_much), UserError, "«Торт»: остаток на 13.10.2026 уйдёт в минус, не хватает 1 кг");
}

/**
 * @brief Возврат в тот же день, но раньше по времени, чем реализация, отклоняется.
 */
TEST_F(ReturnsTest, ReturnNotBeforeSaleTime) {
    auto sale = make_doc(DocType::SALE, "2026-10-14", {{m_bread, 1000}});
    sale.time = "15:00";
    sale.price_type_id = m_v1;
    sale.lines[0].price_kop = 4500;
    auto const id = m_docs.create(sale);
    m_docs.post(id);
    auto ret = make_doc(DocType::RETURN, "2026-10-14", {{m_bread, 1000}});
    ret.base_id = id;
    ret.time = "14:00";
    EXPECT_THROW_REASON(m_docs.create(ret), UserError, "возврат не может быть раньше реализации");
    ret.time = "15:30";
    EXPECT_NO_THROW(m_docs.create(ret));
}
