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
 * @brief Батон: себестоимость 31,20, «Вариант 1» 45,00 с 10.10; торт без цен; на складе 40 батонов и 5 кг торта.
 */
class SalesTest : public DbFixture {
   protected:
    SalesTest()
        : m_prices{m_db},
          m_partners{m_db},
          m_bread{add("Батон", Kind::PRODUCT, Unit::PCS)},
          m_cake{add("Торт", Kind::PRODUCT, Unit::KG)},
          m_v1{m_prices.create_type("Вариант 1")} {
        set_price(COST_PRICE_TYPE, m_bread, 3120);
        set_price(m_v1, m_bread, 4500);
        m_docs.post(m_docs.create(make_doc(DocType::PRODUCTION, "2026-10-11", {{m_bread, 40000}, {m_cake, 5000}})));
    }

    void set_price(std::int64_t type, std::int64_t item, std::int64_t price) {
        Document d;
        d.type = DocType::PRICES;
        d.date = "2026-10-10";
        d.price_type_id = type;
        DocLine l;
        l.item_id = item;
        l.price_kop = price;
        d.lines = {l};
        m_docs.post(m_docs.create(d));
    }

    /// Реализация из троек {позиция, количество, цена или nullopt}.
    Document sale(std::string const &date, std::vector<std::tuple<std::int64_t, std::int64_t, std::optional<std::int64_t>>> const &lines) {
        Document d;
        d.type = DocType::SALE;
        d.date = date;
        d.price_type_id = m_v1;
        for (auto const &[item, qty, price] : lines) {
            DocLine l;
            l.item_id = item;
            l.qty = qty;
            l.price_kop = price;
            d.lines.push_back(l);
        }
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
    Partners m_partners;
    std::int64_t m_bread;
    std::int64_t m_cake;
    std::int64_t m_v1;
};

} // namespace

/**
 * @brief Цена из типа цен на дату, себестоимость на дату; проведение списывает со склада.
 */
TEST_F(SalesTest, PriceFromTypeAndStockDecreases) {
    auto const id = m_docs.create(sale("2026-10-12", {{m_bread, 30000, std::nullopt}}));
    auto const doc = m_docs.get(id);
    EXPECT_EQ(doc.lines[0].price_kop, 4500) << "цена из «Варианта 1» на 12.10";
    EXPECT_EQ(doc.lines[0].cost_kop, 3120) << "себестоимость на 12.10";
    m_docs.post(id);
    EXPECT_EQ(balance(m_bread), 10000) << "40 - 30";
    m_docs.unpost(id);
    EXPECT_EQ(balance(m_bread), 40000);
}

/**
 * @brief Цену, поправленную продавцом, сервер не заменяет типовой.
 */
TEST_F(SalesTest, ManualPriceKept) {
    auto const id = m_docs.create(sale("2026-10-12", {{m_bread, 10000, 4000}}));
    EXPECT_EQ(m_docs.get(id).lines[0].price_kop, 4000);
    EXPECT_THROW_REASON(m_docs.create(sale("2026-10-12", {{m_bread, 1000, -1}})), UserError, "цена должна быть от 0");
}

/**
 * @brief Без цены реализация сохраняется, но не проводится; в тексте все строки без цены.
 */
TEST_F(SalesTest, NoPriceCannotPost) {
    auto const id = m_docs.create(sale("2026-10-12", {{m_bread, 1000, std::nullopt}, {m_cake, 1000, std::nullopt}}));
    EXPECT_FALSE(m_docs.get(id).lines[1].price_kop) << "торта нет в «Варианте 1»";
    EXPECT_THROW_REASON(m_docs.post(id), UserError, "нет цены в типе «Вариант 1» на 12.10.2026: Торт");
    EXPECT_THAT(m_docs.get(id).status, StrEq("draft"));
    EXPECT_EQ(balance(m_cake), 5000);

    auto fixed = sale("2026-10-12", {{m_bread, 1000, std::nullopt}, {m_cake, 1000, 70000}});
    fixed.id = id;
    m_docs.update(fixed);
    EXPECT_NO_THROW(m_docs.post(id)) << "цену торта ввели вручную";
    EXPECT_EQ(balance(m_cake), 4000);
}

/**
 * @brief Перепроведение реализации тоже требует цен и не пускает в минус.
 */
TEST_F(SalesTest, RepostChecks) {
    auto const id = m_docs.create(sale("2026-10-12", {{m_bread, 10000, std::nullopt}}));
    m_docs.post(id);
    auto no_price = sale("2026-10-12", {{m_cake, 1000, std::nullopt}});
    no_price.id = id;
    EXPECT_THROW_REASON(m_docs.update(no_price), UserError, "нет цены");
    auto too_much = sale("2026-10-12", {{m_bread, 45000, std::nullopt}});
    too_much.id = id;
    EXPECT_THROW_REASON(m_docs.update(too_much), UserError, "«Батон»: остаток на 12.10.2026 уйдёт в минус, не хватает 5 шт");
    EXPECT_EQ(m_docs.get(id).lines[0].qty, 10000) << "после отказов документ прежний";
    EXPECT_EQ(balance(m_bread), 30000);
}

/**
 * @brief Продать больше, чем есть, нельзя.
 */
TEST_F(SalesTest, NoNegativeStock) {
    auto const id = m_docs.create(sale("2026-10-12", {{m_cake, 6000, 70000}}));
    EXPECT_THROW_REASON(m_docs.post(id), UserError, "«Торт»: остаток на 12.10.2026 уйдёт в минус, не хватает 1 кг");
    auto const early = m_docs.create(sale("2026-10-10", {{m_bread, 1000, std::nullopt}}));
    // поступление было только 11-го
    EXPECT_THROW_REASON(m_docs.post(early), UserError, "остаток на 10.10.2026 уйдёт в минус");
}

/**
 * @brief Тип цен в реализации обязателен.
 */
TEST_F(SalesTest, PriceTypeRequired) {
    auto d = sale("2026-10-12", {{m_bread, 1000, std::nullopt}});
    d.price_type_id.reset();
    EXPECT_THROW_REASON(m_docs.create(d), UserError, "не выбран тип цен");
}

/**
 * @brief Клиент в заявке и реализации: имя в документе, архивного выбрать нельзя, в поступлении клиента нет.
 */
TEST_F(SalesTest, Partners) {
    Partner p;
    p.name = " Колосок ";
    p.phone = "+7 900 000-00-00";
    auto const shop = m_partners.create(p);
    EXPECT_THAT(m_partners.get(shop).name, StrEq("Колосок"));
    EXPECT_THROW_REASON(m_partners.create(p), UserError, "клиент «Колосок» уже есть");
    p.name = "";
    EXPECT_THROW_REASON(m_partners.create(p), UserError, "не может быть пустым");

    auto order = make_order("2026-10-12", {{m_bread, 1000}});
    order.partner_id = shop;
    auto const order_id = m_docs.create(order);
    EXPECT_THAT(m_docs.get(order_id).partner_name, StrEq("Колосок"));

    auto s = sale("2026-10-12", {{m_bread, 1000, std::nullopt}});
    s.partner_id = shop;
    EXPECT_EQ(m_docs.get(m_docs.create(s)).partner_id, shop);

    auto production = make_doc(DocType::PRODUCTION, "2026-10-12", {{m_bread, 1000}});
    production.partner_id = shop;
    EXPECT_FALSE(m_docs.get(m_docs.create(production)).partner_id) << "у поступления клиента нет";

    EXPECT_THROW_REASON(m_partners.remove(shop), UserError, "есть в документах (2)");
    auto archived = m_partners.get(shop);
    archived.archived = true;
    m_partners.update(archived);
    EXPECT_THROW_REASON(m_docs.create(order), UserError, "клиент «Колосок» в архиве");
    auto same = order;
    same.id = order_id;
    EXPECT_NO_THROW(m_docs.update(same)) << "в документе, где клиент уже был, архив не мешает";
    EXPECT_EQ(m_partners.list(false).size(), 0U);
    EXPECT_EQ(m_partners.list(true).size(), 1U);
}
