#include "prices.hpp"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <sstream>

#include "errors.hpp"
#include "fixture.hpp"
#include "test_util.hpp"

using testing::ElementsAre;
using testing::StrEq;

namespace {

/**
 * @brief Батон (шт) и торт (кг), тип цен «Вариант 1».
 */
class PricesTest : public DbFixture {
   protected:
    PricesTest()
        : m_prices{m_db},
          m_bread{add("Батон", Kind::PRODUCT, Unit::PCS)},
          m_cake{add("Торт", Kind::PRODUCT, Unit::KG)},
          m_v1{m_prices.create_type("Вариант 1")} {}

    /// Установка цен из пар {позиция, цена в копейках}; проводится, если post.
    std::int64_t set_prices(std::int64_t type, std::string const &date, std::vector<std::pair<std::int64_t, std::int64_t>> const &lines,
                            bool post = true) {
        Document d;
        d.type = DocType::PRICES;
        d.date = date;
        d.price_type_id = type;
        for (auto const &[item, price] : lines) {
            DocLine l;
            l.item_id = item;
            l.price_kop = price;
            d.lines.push_back(l);
        }
        auto const id = m_docs.create(d);
        if (post) {
            m_docs.post(id);
        }
        return id;
    }

    std::optional<std::int64_t> price(std::int64_t type, std::int64_t item, std::string const &date) const {
        return m_prices.price(type, item, date);
    }

    Prices m_prices;
    std::int64_t m_bread;
    std::int64_t m_cake;
    std::int64_t m_v1;
};

} // namespace

/**
 * @brief Встроенная «Себестоимость» есть всегда, её нельзя переименовать и удалить; свои типы уникальны.
 */
TEST_F(PricesTest, Types) {
    auto const t = m_prices.types();
    ASSERT_EQ(t.size(), 2U);
    EXPECT_THAT(t[0].name, StrEq("Себестоимость"));
    EXPECT_TRUE(t[0].system);
    EXPECT_EQ(t[0].id, COST_PRICE_TYPE);

    EXPECT_THROW_REASON(m_prices.create_type(" Вариант 1 "), UserError, "тип цен «Вариант 1» уже есть");
    EXPECT_THROW_REASON(m_prices.create_type(""), UserError, "не может быть пустым");
    EXPECT_THROW_REASON(m_prices.rename_type(COST_PRICE_TYPE, "Закупка"), UserError, "встроенный тип цен переименовать нельзя");
    EXPECT_THROW_REASON(m_prices.remove_type(COST_PRICE_TYPE), UserError, "встроенный тип цен «Себестоимость» удалить нельзя");

    m_prices.rename_type(m_v1, "Опт");
    EXPECT_THAT(m_prices.type(m_v1).name, StrEq("Опт"));
    set_prices(m_v1, "2026-10-10", {{m_bread, 4500}}, false);
    EXPECT_THROW_REASON(m_prices.remove_type(m_v1), UserError, "используется в документах (1)");
    auto const v2 = m_prices.create_type("Розница");
    EXPECT_NO_THROW(m_prices.remove_type(v2));
}

/**
 * @brief Цена на дату - из последней проведённой установки не позже даты; черновик не действует.
 */
TEST_F(PricesTest, PriceByDate) {
    set_prices(m_v1, "2026-10-10", {{m_bread, 4500}, {m_cake, 69000}});
    auto const rise = set_prices(m_v1, "2026-10-15", {{m_bread, 4700}});
    set_prices(m_v1, "2026-10-12", {{m_bread, 9900}}, false);

    EXPECT_FALSE(price(m_v1, m_bread, "2026-10-09")) << "до первой установки цены нет";
    EXPECT_EQ(price(m_v1, m_bread, "2026-10-12"), 4500) << "черновик от 12-го не действует";
    EXPECT_EQ(price(m_v1, m_bread, "2026-10-15"), 4700) << "с 15-го новая цена";
    EXPECT_EQ(price(m_v1, m_cake, "2026-10-20"), 69000) << "торт не менялся 15-го, цена от 10-го";

    m_docs.unpost(rise);
    EXPECT_EQ(price(m_v1, m_bread, "2026-10-20"), 4500) << "после отмены проведения действует прежняя цена";
}

/**
 * @brief Две установки на одну дату: действует проведённая позже (больший id).
 */
TEST_F(PricesTest, SameDateLaterDocumentWins) {
    set_prices(m_v1, "2026-10-10", {{m_bread, 4500}});
    set_prices(m_v1, "2026-10-10", {{m_bread, 4600}});
    EXPECT_EQ(price(m_v1, m_bread, "2026-10-10"), 4600);
}

/**
 * @brief Типы цен независимы, товар может быть в одном типе и отсутствовать в другом.
 */
TEST_F(PricesTest, TypesAreIndependent) {
    auto const v2 = m_prices.create_type("Вариант 2");
    set_prices(m_v1, "2026-10-10", {{m_bread, 4500}, {m_cake, 69000}});
    set_prices(v2, "2026-10-10", {{m_bread, 4200}});
    EXPECT_EQ(price(v2, m_bread, "2026-10-11"), 4200);
    EXPECT_FALSE(price(v2, m_cake, "2026-10-11")) << "торта нет в «Варианте 2»";

    std::vector<std::string> names;
    for (auto const &row : m_prices.price_list(m_v1, "2026-10-11")) {
        names.push_back(row.name + "=" + std::to_string(row.price_kop));
    }
    EXPECT_THAT(names, ElementsAre("Батон=4500", "Торт=69000"));
}

/**
 * @brief Проверки установки цен: тип цен, цена в каждой строке, границы цены.
 */
TEST_F(PricesTest, Validation) {
    Document d;
    d.type = DocType::PRICES;
    d.date = "2026-10-10";
    DocLine l;
    l.item_id = m_bread;
    d.lines = {l};
    EXPECT_THROW_REASON(m_docs.create(d), UserError, "не выбран тип цен");
    d.price_type_id = 999;
    EXPECT_THROW_REASON(m_docs.create(d), NotFoundError, "нет типа цен с id 999");
    d.price_type_id = m_v1;
    EXPECT_THROW_REASON(m_docs.create(d), UserError, "Батон: не указана цена");
    d.lines[0].price_kop = -1;
    EXPECT_THROW_REASON(m_docs.create(d), UserError, "цена должна быть от 0");
    d.lines[0].price_kop = 0;
    EXPECT_NO_THROW(m_docs.create(d)) << "нулевая цена допустима";
}

/**
 * @brief Себестоимость в строках поступления: на дату документа при сохранении, пусто если не установлена;
 * изменение себестоимости позже не меняет сохранённый документ, пока его не пересохранят.
 */
TEST_F(PricesTest, CostInProductionLines) {
    set_prices(COST_PRICE_TYPE, "2026-10-10", {{m_bread, 3120}});
    auto const early = m_docs.create(make_doc(DocType::PRODUCTION, "2026-10-09", {{m_bread, 1000}}));
    auto const id = m_docs.create(make_doc(DocType::PRODUCTION, "2026-10-12", {{m_bread, 40000}, {m_cake, 2500}}));
    EXPECT_FALSE(m_docs.get(early).lines[0].cost_kop) << "9-го себестоимость ещё не установлена";
    auto doc = m_docs.get(id);
    EXPECT_EQ(doc.lines[0].cost_kop, 3120);
    EXPECT_FALSE(doc.lines[1].cost_kop) << "у торта себестоимости нет";
    EXPECT_FALSE(doc.lines[0].price_kop) << "в поступлении цены продажи нет";

    set_prices(COST_PRICE_TYPE, "2026-10-11", {{m_bread, 3300}});
    EXPECT_EQ(m_docs.get(id).lines[0].cost_kop, 3120) << "сохранённая себестоимость не меняется сама";
    m_docs.post(id);
    auto resave = make_doc(DocType::PRODUCTION, "2026-10-12", {{m_bread, 40000}});
    resave.id = id;
    m_docs.update(resave);
    EXPECT_EQ(m_docs.get(id).lines[0].cost_kop, 3300) << "при перепроведении себестоимость берётся заново на дату";
}

/**
 * @brief Тип цен игнорируется у видов документов, где он не нужен.
 */
TEST_F(PricesTest, PriceTypeOnlyWhereNeeded) {
    auto order = make_order("2026-10-12", {{m_bread, 1000}});
    order.price_type_id = m_v1;
    EXPECT_FALSE(m_docs.get(m_docs.create(order)).price_type_id);
}
