#include "requirements.hpp"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <sstream>

#include "errors.hpp"
#include "fixture.hpp"
#include "test_util.hpp"

using testing::ElementsAre;
using testing::StrEq;

namespace {

/// Список потребностей строкой "Название=кол-во; ..." для понятного сравнения.
std::string dump(std::vector<Need> const &list) {
    std::ostringstream out;
    for (auto const &n : list) {
        out << n.name << "=" << n.qty << "; ";
    }
    return out.str();
}

/**
 * @brief Пекарня: тесто на 10 кг (мука 6, вода 3,5 л, дрожжи 0,15), батон 1 шт из 0,45 кг теста и яйца.
 */
class RequirementsTest : public DbFixture {
   protected:
    RequirementsTest()
        : m_flour{add("Мука", Kind::RAW)},
          m_water{add("Вода", Kind::RAW, Unit::L)},
          m_yeast{add("Дрожжи", Kind::RAW)},
          m_egg{add("Яйцо", Kind::RAW, Unit::PCS)},
          m_dough{add("Тесто", Kind::SEMI)},
          m_bread{add("Батон", Kind::PRODUCT, Unit::PCS)} {
        m_catalog.set_recipe(m_dough, make_recipe(10000, {{m_flour, 6000}, {m_water, 3500}, {m_yeast, 150}}));
        m_catalog.set_recipe(m_bread, make_recipe(1000, {{m_dough, 450}, {m_egg, 100}}));
    }

    std::int64_t m_flour, m_water, m_yeast, m_egg, m_dough, m_bread;
};

} // namespace

/**
 * @brief Две заявки на батоны суммируются и разворачиваются через тесто до сырья.
 *
 * 30 батонов: тесто 30 × 0,45 = 13,5 кг; мука 13,5 × 0,6 = 8,1 кг; вода 4,725 л; дрожжи 0,2025 → 0,203 кг
 * (вверх); яйца 3 шт.
 */
TEST_F(RequirementsTest, TwoOrdersThroughSemi) {
    auto const a = m_docs.create(make_order("2026-10-12", {{m_bread, 10000}}));
    auto const b = m_docs.create(make_order("2026-10-12", {{m_bread, 20000}}));
    auto const r = compute_requirements(m_db, {a, b});
    EXPECT_THAT(dump(r.products), StrEq("Батон=30000; "));
    EXPECT_THAT(dump(r.intermediates), StrEq("Тесто=13500; "));
    EXPECT_THAT(dump(r.raw), StrEq("Вода=4725; Дрожжи=203; Мука=8100; Яйцо=3000; ")) << "дрожжи 202,5 г округляются вверх до 203";
    EXPECT_TRUE(r.warnings.empty()) << r.warnings.size();
}

/**
 * @brief Штучное сырьё округляется вверх до целых только в итоге, после суммирования.
 *
 * 15 батонов по 0,1 яйца = 1,5 → 2 яйца; 5 батонов в другой заявке не дают отдельного округления.
 */
TEST_F(RequirementsTest, PiecesRoundedUpInTotal) {
    auto const a = m_docs.create(make_order("2026-10-12", {{m_bread, 15000}}));
    auto const b = m_docs.create(make_order("2026-10-12", {{m_bread, 5000}}));
    EXPECT_EQ(compute_requirements(m_db, {a}).raw[3].qty, 2000) << "1,5 яйца → 2";
    EXPECT_EQ(compute_requirements(m_db, {a, b}).raw[3].qty, 2000) << "20 батонов → ровно 2 яйца, без двойного округления";
}

/**
 * @brief Спрос на общий полуфабрикат суммируется со всех продуктов до округления.
 *
 * Тесто: на 3 кг муки 1 кг. Пирожок и ватрушка берут по 0,001 кг теста, всего 0,002 кг.
 * Мука по сумме: ceil(0,002 × 1 / 3) = 0,001 кг. Если округлять каждый путь отдельно,
 * вышло бы 0,001 + 0,001 = 0,002 кг.
 */
TEST_F(RequirementsTest, SharedSemiRoundedOnce) {
    auto const flour = add("Мука 2", Kind::RAW);
    auto const dough = add("Тесто 2", Kind::SEMI);
    auto const pie = add("Пирожок", Kind::PRODUCT, Unit::PCS);
    auto const bun = add("Ватрушка", Kind::PRODUCT, Unit::PCS);
    m_catalog.set_recipe(dough, make_recipe(3000, {{flour, 1000}}));
    m_catalog.set_recipe(pie, make_recipe(1000, {{dough, 1}}));
    m_catalog.set_recipe(bun, make_recipe(1000, {{dough, 1}}));
    auto const id = m_docs.create(make_order("2026-10-12", {{pie, 1000}, {bun, 1000}}));
    auto const r = compute_requirements(m_db, {id});
    EXPECT_THAT(dump(r.intermediates), StrEq("Тесто 2=2; "));
    EXPECT_THAT(dump(r.raw), StrEq("Мука 2=1; ")) << "округление должно быть одно, по сумме спроса на тесто";
}

/**
 * @brief Продукция, которая и заказана, и входит в другую продукцию, разворачивается по полному спросу.
 *
 * Бисквит: 1 кг из 0,5 кг муки. Заказ: бисквит 1 кг и 2 торта по 0,4 кг бисквита. Мука на 1,8 кг бисквита = 0,9 кг.
 */
TEST_F(RequirementsTest, ProductOrderedAndComponent) {
    auto const sponge = add("Бисквит", Kind::PRODUCT, Unit::KG);
    auto const cake = add("Торт", Kind::PRODUCT, Unit::PCS);
    m_catalog.set_recipe(sponge, make_recipe(1000, {{m_flour, 500}}));
    m_catalog.set_recipe(cake, make_recipe(1000, {{sponge, 400}}));
    auto const id = m_docs.create(make_order("2026-10-12", {{sponge, 1000}, {cake, 2000}}));
    auto const r = compute_requirements(m_db, {id});
    EXPECT_THAT(dump(r.products), StrEq("Бисквит=1000; Торт=2000; "));
    EXPECT_THAT(dump(r.intermediates), StrEq("Бисквит=800; ")) << "бисквит в составе тортов";
    EXPECT_THAT(dump(r.raw), StrEq("Мука=900; "));
}

/**
 * @brief Позиция без рецептуры остаётся в списке и даёт предупреждение.
 */
TEST_F(RequirementsTest, MissingRecipeWarning) {
    auto const cookie = add("Печенье", Kind::PRODUCT, Unit::KG);
    auto const id = m_docs.create(make_order("2026-10-12", {{cookie, 2000}, {m_bread, 1000}}));
    auto const r = compute_requirements(m_db, {id});
    EXPECT_THAT(dump(r.products), StrEq("Батон=1000; Печенье=2000; "));
    EXPECT_THAT(r.warnings, ElementsAre("«Печенье»: нет рецептуры, сырьё для неё не рассчитано"));
    EXPECT_FALSE(r.products[1].has_recipe);
}

/**
 * @brief Повтор id не удваивает спрос; пустой список и несуществующая заявка отклоняются.
 */
TEST_F(RequirementsTest, Errors) {
    auto const id = m_docs.create(make_order("2026-10-12", {{m_bread, 1000}}));
    EXPECT_THAT(dump(compute_requirements(m_db, {id, id}).products), StrEq("Батон=1000; ")) << "id передан дважды";
    EXPECT_THROW_REASON((void)compute_requirements(m_db, {}), UserError, "не выбрано ни одной заявки");
    EXPECT_THROW_REASON((void)compute_requirements(m_db, {404}), NotFoundError, "id 404");
}

/**
 * @brief Большие количества не переполняют промежуточные произведения.
 */
TEST_F(RequirementsTest, LargeQuantities) {
    auto const id = m_docs.create(make_order("2026-10-12", {{m_bread, 1'000'000'000}}));
    auto const r = compute_requirements(m_db, {id});
    EXPECT_THAT(dump(r.intermediates), StrEq("Тесто=450000000; ")) << "миллион батонов × 0,45 кг";
    EXPECT_THAT(r.raw[2].name, testing::StrEq("Мука"));
    EXPECT_EQ(r.raw[2].qty, 270'000'000);
}
