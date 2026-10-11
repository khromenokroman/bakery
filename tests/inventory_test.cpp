#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <sstream>

#include "errors.hpp"
#include "fixture.hpp"
#include "groups.hpp"
#include "prices.hpp"
#include "report.hpp"
#include "stock.hpp"
#include "test_util.hpp"

using testing::StrEq;

namespace {

/**
 * @brief Группы «Хлеб» (батон) и «Торты» (наполеон, эклер). 10.10 в 08:00 поступило: батон 10 шт,
 * наполеон 5 кг, эклер 20 шт. Себестоимость батона 30,00.
 */
class InventoryTest : public DbFixture {
   protected:
    InventoryTest() : m_groups{m_db} {
        Group g;
        g.name = "Хлеб";
        m_bread_group = m_groups.create(g);
        g.name = "Торты";
        m_cakes = m_groups.create(g);
        m_bread = item("Батон", m_bread_group, Unit::PCS);
        m_napoleon = item("Наполеон", m_cakes, Unit::KG);
        m_eclair = item("Эклер", m_cakes, Unit::PCS);

        Document cost;
        cost.type = DocType::PRICES;
        cost.date = "2026-10-01";
        cost.price_type_id = COST_PRICE_TYPE;
        DocLine l;
        l.item_id = m_bread;
        l.price_kop = 3000;
        cost.lines = {l};
        m_docs.post(m_docs.create(cost));

        auto prod = make_doc(DocType::PRODUCTION, "2026-10-10", {{m_bread, 10000}, {m_napoleon, 5000}, {m_eclair, 20000}});
        prod.time = "08:00";
        m_docs.post(m_docs.create(prod));
    }

    std::int64_t item(std::string const &name, std::int64_t group, Unit unit) {
        Item i;
        i.name = name;
        i.kind = Kind::PRODUCT;
        i.unit = unit;
        i.group_id = group;
        return m_catalog.create(i);
    }

    Document inventory(std::string const &date, std::vector<std::pair<std::int64_t, std::int64_t>> const &lines,
                       std::optional<std::int64_t> group = std::nullopt) {
        auto d = make_doc(DocType::INVENTORY, date, lines);
        d.time = "20:00";
        d.group_id = group;
        return d;
    }

    std::string balances(std::optional<std::string> const &date = std::nullopt) {
        std::ostringstream out;
        for (auto const &b : Stock{m_db}.balances(date)) {
            out << b.name << "=" << b.qty << "; ";
        }
        return out.str();
    }

    Groups m_groups;
    std::int64_t m_bread_group{}, m_cakes{}, m_bread{}, m_napoleon{}, m_eclair{};
};

} // namespace

/**
 * @brief По всей продукции: насчитанное становится остатком, неучтённые позиции обнуляются.
 */
TEST_F(InventoryTest, WholeStockKeepsOnlyCounted) {
    auto const id = m_docs.create(inventory("2026-10-10", {{m_bread, 8000}, {m_eclair, 22000}}));
    m_docs.post(id);
    EXPECT_THAT(balances(), StrEq("Батон=8000; Эклер=22000; ")) << "наполеона нет в документе - обнулён; батон -2, эклер +2";
    m_docs.unpost(id);
    EXPECT_THAT(balances(), StrEq("Батон=10000; Наполеон=5000; Эклер=20000; ")) << "отмена проведения возвращает учёт";
}

/**
 * @brief По группе: обнуляются только неучтённые позиции этой группы, остальные не трогаются.
 */
TEST_F(InventoryTest, GroupScope) {
    m_docs.post(m_docs.create(inventory("2026-10-10", {{m_napoleon, 4500}}, m_cakes)));
    EXPECT_THAT(balances(), StrEq("Батон=10000; Наполеон=4500; ")) << "эклер (Торты) обнулён, батон (Хлеб) не тронут";
    EXPECT_THROW_REASON(m_docs.create(inventory("2026-10-10", {{m_bread, 1000}}, m_cakes)), UserError,
                        "«Батон» не входит в группу инвентаризации «Торты»");
}

/**
 * @brief Нулевой факт допустим, отрицательный - нет; документ без строк по группе - тоже ошибка (нет строк).
 */
TEST_F(InventoryTest, Validation) {
    EXPECT_NO_THROW(m_docs.post(m_docs.create(inventory("2026-10-10", {{m_eclair, 0}}, m_cakes))));
    EXPECT_THAT(balances(), StrEq("Батон=10000; "));
    EXPECT_THROW_REASON(m_docs.create(inventory("2026-10-10", {{m_bread, -1}})), UserError, "количество не может быть отрицательным");
}

/**
 * @brief Учёт берётся на момент документа: продажа позже инвентаризации не влияет на её разницу,
 * а инвентаризация задним числом, после которой более поздний расход уходит в минус, отклоняется.
 */
TEST_F(InventoryTest, MomentAndLaterDocuments) {
    m_docs.post(m_docs.create(make_doc(DocType::WRITEOFF, "2026-10-11", {{m_bread, 6000}}, "брак")));
    auto const id = m_docs.create(inventory("2026-10-10", {{m_bread, 5000}}, m_bread_group));
    EXPECT_THROW_REASON(m_docs.post(id), UserError, "«Батон»: остаток на 11.10.2026 уйдёт в минус, не хватает 1 шт");
    auto fixed = inventory("2026-10-10", {{m_bread, 7000}}, m_bread_group);
    fixed.id = id;
    m_docs.update(fixed);
    m_docs.post(id);
    EXPECT_THAT(balances("2026-10-10"), StrEq("Батон=7000; Наполеон=5000; Эклер=20000; "));
    EXPECT_THAT(balances(), StrEq("Батон=1000; Наполеон=5000; Эклер=20000; ")) << "7 - 6 списано 11-го";
}

/**
 * @brief Перепроведение пересчитывает разницу; повторная инвентаризация с тем же фактом не даёт движений.
 */
TEST_F(InventoryTest, RepostAndIdempotent) {
    auto const id = m_docs.create(inventory("2026-10-10", {{m_bread, 9000}}, m_bread_group));
    m_docs.post(id);
    auto changed = inventory("2026-10-10", {{m_bread, 12000}}, m_bread_group);
    changed.id = id;
    m_docs.update(changed);
    EXPECT_THAT(balances(), StrEq("Батон=12000; Наполеон=5000; Эклер=20000; ")) << "учёт без движений самого документа";

    auto again = inventory("2026-10-11", {{m_bread, 12000}}, m_bread_group);
    auto const second = m_docs.create(again);
    m_docs.post(second);
    auto stmt = m_db.prepare("SELECT count(*) FROM stock_moves WHERE doc_id = ?");
    stmt.bind(1, second);
    stmt.step();
    EXPECT_EQ(stmt.column_int(0), 0) << "факт совпал с учётом - движений нет";
}

/**
 * @brief В товарном отчёте излишки - приход, недостачи - расход, себестоимость на дату документа.
 */
TEST_F(InventoryTest, InGoodsReport) {
    m_docs.post(m_docs.create(inventory("2026-10-11", {{m_bread, 8000}, {m_napoleon, 5000}, {m_eclair, 21000}})));
    auto const r = goods_report(m_db, "2026-10-11", "2026-10-11");
    ASSERT_EQ(r.incoming.size(), 1U);
    ASSERT_EQ(r.outgoing.size(), 1U);
    EXPECT_EQ(r.incoming[0].type, DocType::INVENTORY);
    EXPECT_EQ(r.outgoing[0].sum, 6000) << "недостача 2 батона × 30,00";
    EXPECT_EQ(r.incoming[0].sum, 0) << "излишек эклера без себестоимости";
}
