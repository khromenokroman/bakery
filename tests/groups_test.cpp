#include "groups.hpp"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <sstream>

#include "errors.hpp"
#include "fixture.hpp"
#include "stock.hpp"
#include "test_util.hpp"

using testing::ElementsAre;
using testing::StrEq;

namespace {

/**
 * @brief Дерево: Хлеб (Батоны), Кондитерка (Торты, Пирожные), Сырьё.
 */
class GroupsTest : public DbFixture {
   protected:
    GroupsTest()
        : m_groups{m_db},
          m_bread{group("Хлеб")},
          m_loaves{group("Батоны", m_bread)},
          m_pastry{group("Кондитерка")},
          m_cakes{group("Торты", m_pastry)},
          m_desserts{group("Пирожные", m_pastry)} {}

    std::int64_t group(std::string const &name, std::optional<std::int64_t> parent = std::nullopt) {
        Group g;
        g.name = name;
        g.parent_id = parent;
        return m_groups.create(g);
    }

    std::int64_t item_in(std::string const &name, std::optional<std::int64_t> group_id, Unit unit = Unit::PCS) {
        Item item;
        item.name = name;
        item.kind = Kind::PRODUCT;
        item.unit = unit;
        item.group_id = group_id;
        return m_catalog.create(item);
    }

    std::vector<std::string> item_names(std::int64_t group_id) const {
        std::vector<std::string> out;
        for (auto const &i : m_catalog.list({.group = group_id})) {
            out.push_back(i.name);
        }
        return out;
    }

    Groups m_groups;
    std::int64_t m_bread, m_loaves, m_pastry, m_cakes, m_desserts;
};

} // namespace

/**
 * @brief Путь группы от корня и подгруппы на любой глубине.
 */
TEST_F(GroupsTest, PathAndDescendants) {
    auto const cream = group("Кремовые", m_cakes);
    EXPECT_THAT(m_groups.path(cream), StrEq("Кондитерка / Торты / Кремовые"));
    EXPECT_EQ(m_groups.with_descendants(m_pastry), (std::set<std::int64_t>{m_pastry, m_cakes, m_desserts, cream}));
    EXPECT_EQ(m_groups.with_descendants(m_loaves), (std::set<std::int64_t>{m_loaves}));
    EXPECT_EQ(m_groups.list().size(), 6U);
}

/**
 * @brief Название уникально среди соседей: два «Торты» в одной группе нельзя, в разных можно.
 */
TEST_F(GroupsTest, NameUniqueAmongSiblings) {
    EXPECT_THROW_REASON(group("Торты", m_pastry), UserError, "группа «Торты» здесь уже есть");
    // и на верхнем уровне
    EXPECT_THROW_REASON(group(" Хлеб "), UserError, "группа «Хлеб» здесь уже есть");
    EXPECT_NO_THROW(group("Торты", m_bread)) << "в другой группе такое название допустимо";
    EXPECT_THROW_REASON(group("  "), UserError, "название группы не может быть пустым");
    EXPECT_THROW_REASON(group("X", 999), NotFoundError, "нет группы с id 999");
}

/**
 * @brief Группу нельзя вложить в саму себя или в свою подгруппу; перенос в другую ветку работает.
 */
TEST_F(GroupsTest, MoveRules) {
    Group g = m_groups.get(m_pastry);
    g.parent_id = m_cakes;
    EXPECT_THROW_REASON(m_groups.update(g), UserError, "нельзя вложить в саму себя или в её подгруппу");
    g.parent_id = m_pastry;
    EXPECT_THROW_REASON(m_groups.update(g), UserError, "нельзя вложить в саму себя");

    Group cakes = m_groups.get(m_cakes);
    cakes.parent_id = std::nullopt;
    cakes.name = "Торты на заказ";
    m_groups.update(cakes);
    EXPECT_THAT(m_groups.path(m_cakes), StrEq("Торты на заказ")) << "вынесли на верхний уровень и переименовали";
    EXPECT_EQ(m_groups.with_descendants(m_pastry), (std::set<std::int64_t>{m_pastry, m_desserts}));
}

/**
 * @brief Удалить можно только пустую группу.
 */
TEST_F(GroupsTest, RemoveOnlyEmpty) {
    EXPECT_THROW_REASON(m_groups.remove(m_pastry), UserError, "в группе «Кондитерка» есть подгруппы");
    item_in("Наполеон", m_cakes);
    EXPECT_THROW_REASON(m_groups.remove(m_cakes), UserError, "в группе «Торты» есть позиции (1)");
    EXPECT_NO_THROW(m_groups.remove(m_desserts));
    EXPECT_THROW_REASON((void)m_groups.get(m_desserts), NotFoundError, "нет группы");
}

/**
 * @brief Список номенклатуры по группе включает все подгруппы; позицию можно перенести и убрать из группы.
 */
TEST_F(GroupsTest, ItemsFilterWithSubgroups) {
    item_in("Наполеон", m_cakes);
    item_in("Эклер", m_desserts);
    auto const loaf = item_in("Батон", m_loaves);
    item_in("Без группы", std::nullopt);
    EXPECT_THAT(item_names(m_pastry), ElementsAre("Наполеон", "Эклер")) << "Кондитерка = Торты + Пирожные";
    EXPECT_THAT(item_names(m_cakes), ElementsAre("Наполеон"));
    EXPECT_THAT(item_names(m_bread), ElementsAre("Батон"));

    auto item = m_catalog.get(loaf);
    EXPECT_EQ(item.group_id, m_loaves);
    item.group_id = std::nullopt;
    m_catalog.update(item);
    EXPECT_FALSE(m_catalog.get(loaf).group_id) << "позиция убрана из группы";
    EXPECT_TRUE(item_names(m_bread).empty());

    item.group_id = 999;
    EXPECT_THROW_REASON(m_catalog.update(item), NotFoundError, "нет группы с id 999");
}

/**
 * @brief Остатки по группе включают подгруппы.
 */
TEST_F(GroupsTest, StockByGroup) {
    auto const napoleon = item_in("Наполеон", m_cakes, Unit::KG);
    auto const eclair = item_in("Эклер", m_desserts);
    auto const loaf = item_in("Батон", m_loaves);
    auto const id = m_docs.create(make_doc(DocType::PRODUCTION, "2026-10-12", {{napoleon, 2500}, {eclair, 20000}, {loaf, 40000}}));
    m_docs.post(id);

    Stock const stock{m_db};
    auto const names = [&](std::optional<std::int64_t> group) {
        std::ostringstream out;
        for (auto const &b : stock.balances(std::nullopt, group)) {
            out << b.name << "=" << b.qty << "; ";
        }
        return out.str();
    };
    EXPECT_THAT(names(std::nullopt), StrEq("Батон=40000; Наполеон=2500; Эклер=20000; "));
    EXPECT_THAT(names(m_pastry), StrEq("Наполеон=2500; Эклер=20000; ")) << "Кондитерка с подгруппами";
    EXPECT_THAT(names(m_desserts), StrEq("Эклер=20000; "));
    EXPECT_THAT(names(m_loaves), StrEq("Батон=40000; "));
}
