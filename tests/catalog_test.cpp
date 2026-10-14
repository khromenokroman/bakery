#include "catalog.hpp"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "db.hpp"
#include "errors.hpp"
#include "fixture.hpp"
#include "schema.hpp"
#include "test_util.hpp"

using testing::ElementsAre;

namespace {

class CatalogTest : public DbFixture {
   protected:
    std::vector<std::string> names(ItemFilter const &filter = {}) {
        std::vector<std::string> out;
        for (auto const &i : m_catalog.list(filter)) {
            out.push_back(i.name);
        }
        return out;
    }
};

} // namespace

/**
 * @brief Список идёт по видам (продукция, полуфабрикаты, сырьё), внутри по названию; фильтр по виду работает.
 */
TEST_F(CatalogTest, ListOrderAndKindFilter) {
    add("Мука", Kind::RAW);
    add("Батон", Kind::PRODUCT, Unit::PCS);
    add("Тесто", Kind::SEMI);
    add("Багет", Kind::PRODUCT, Unit::PCS);
    EXPECT_THAT(names(), ElementsAre("Багет", "Батон", "Тесто", "Мука"));
    EXPECT_THAT(names({.kind = Kind::SEMI}), ElementsAre("Тесто")) << "фильтр kind=semi";
}

/**
 * @brief Название обрезается по краям; пустое и повторное отклоняются с понятной причиной.
 */
TEST_F(CatalogTest, CreateValidatesName) {
    auto const id = add("  Мука  ", Kind::RAW);
    EXPECT_THAT(m_catalog.get(id).name, testing::StrEq("Мука")) << "пробелы по краям должны обрезаться";
    EXPECT_THROW_REASON(add("Мука", Kind::RAW), UserError, "«Мука» уже есть");
    EXPECT_THROW_REASON(add(" Мука ", Kind::SEMI), UserError, "«Мука» уже есть");
    EXPECT_THROW_REASON(add("   ", Kind::RAW), UserError, "название не может быть пустым");
    EXPECT_THROW_REASON(add(std::string(201, 'x'), Kind::RAW), UserError, "длиннее 200");
}

/**
 * @brief Архивные позиции скрыты из списка по умолчанию и видны с with_archived.
 */
TEST_F(CatalogTest, ArchiveHidesFromList) {
    auto const id = add("Изюм", Kind::RAW);
    add("Мука", Kind::RAW);
    auto item = m_catalog.get(id);
    item.archived = true;
    m_catalog.update(item);
    EXPECT_THAT(names(), ElementsAre("Мука")) << "архивный «Изюм» не должен попадать в список по умолчанию";
    EXPECT_THAT(names({.with_archived = true}), ElementsAre("Изюм", "Мука"));
}

/**
 * @brief Переименование в занятое название отклоняется, в своё же разрешено.
 */
TEST_F(CatalogTest, UpdateRename) {
    add("Мука", Kind::RAW);
    auto const id = add("Сахар", Kind::RAW);
    auto item = m_catalog.get(id);
    item.name = "Мука";
    EXPECT_THROW_REASON(m_catalog.update(item), UserError, "«Мука» уже есть");
    item.name = "Сахар";
    EXPECT_NO_THROW(m_catalog.update(item)) << "сохранение без смены названия";
    item.name = "Сахар-песок";
    m_catalog.update(item);
    EXPECT_THAT(m_catalog.get(id).name, testing::StrEq("Сахар-песок"));
}

/**
 * @brief Единицу нельзя менять, когда в старой единице уже записаны количества.
 */
TEST_F(CatalogTest, UnitChangeBlockedWhenUsed) {
    auto const flour = add("Мука", Kind::RAW);
    auto const dough = add("Тесто", Kind::SEMI);
    auto const salt = add("Соль", Kind::RAW);
    m_catalog.set_recipe(dough, make_recipe(1000, {{flour, 600}}));

    auto f = m_catalog.get(flour);
    f.unit = Unit::PCS;
    EXPECT_THROW_REASON(m_catalog.update(f), UserError, "«Мука» входит в рецептуры (1)");

    auto d = m_catalog.get(dough);
    d.unit = Unit::L;
    EXPECT_THROW_REASON(m_catalog.update(d), UserError, "есть рецептура в единицах kg");

    auto s = m_catalog.get(salt);
    s.unit = Unit::PCS;
    EXPECT_NO_THROW(m_catalog.update(s)) << "соль нигде не используется, единицу менять можно";
    EXPECT_EQ(m_catalog.get(salt).unit, Unit::PCS);
}

/**
 * @brief Позицию с рецептурой нельзя сделать сырьём.
 */
TEST_F(CatalogTest, KindRawBlockedWithRecipe) {
    auto const flour = add("Мука", Kind::RAW);
    auto const dough = add("Тесто", Kind::SEMI);
    m_catalog.set_recipe(dough, make_recipe(1000, {{flour, 600}}));
    auto d = m_catalog.get(dough);
    d.kind = Kind::RAW;
    EXPECT_THROW_REASON(m_catalog.update(d), UserError, "сырьём её сделать нельзя");
}

/**
 * @brief Удаление: неиспользуемая позиция удаляется вместе с рецептурой, используемая называет, где она нужна.
 */
TEST_F(CatalogTest, Remove) {
    auto const flour = add("Мука", Kind::RAW);
    auto const dough = add("Тесто", Kind::SEMI);
    auto const bread = add("Батон", Kind::PRODUCT, Unit::PCS);
    m_catalog.set_recipe(dough, make_recipe(1000, {{flour, 600}}));
    m_catalog.set_recipe(bread, make_recipe(1000, {{dough, 450}}));

    EXPECT_THROW_REASON(m_catalog.remove(flour), UserError, "«Мука» входит в рецептуры: Тесто");
    m_catalog.remove(bread);
    EXPECT_THROW_REASON((void)m_catalog.get(bread), NotFoundError, "нет позиции");
    EXPECT_FALSE(m_catalog.recipe(bread)) << "рецептура удалённой позиции должна удалиться";
    EXPECT_NO_THROW(m_catalog.remove(dough)) << "тесто больше никуда не входит";
    EXPECT_THROW_REASON(m_catalog.remove(999), NotFoundError, "id 999");
}

/**
 * @brief Рецептура сохраняется и читается с названиями и единицами компонентов, замена полная.
 */
TEST_F(CatalogTest, RecipeRoundTrip) {
    auto const flour = add("Мука", Kind::RAW);
    auto const water = add("Вода", Kind::RAW, Unit::L);
    auto const eggs = add("Яйцо", Kind::RAW, Unit::PCS);
    auto const dough = add("Тесто", Kind::SEMI);
    EXPECT_FALSE(m_catalog.recipe(dough)) << "у новой позиции рецептуры нет";
    EXPECT_FALSE(m_catalog.get(dough).has_recipe);

    m_catalog.set_recipe(dough, make_recipe(10000, {{flour, 6000}, {water, 3500}}));
    auto r = m_catalog.recipe(dough);
    ASSERT_TRUE(r);
    EXPECT_EQ(r->output_qty, 10000);
    ASSERT_EQ(r->lines.size(), 2U);
    EXPECT_THAT(r->lines[0].name, testing::StrEq("Мука"));
    EXPECT_EQ(r->lines[0].qty, 6000);
    EXPECT_EQ(r->lines[1].unit, Unit::L) << "единица компонента «Вода»";
    EXPECT_TRUE(m_catalog.get(dough).has_recipe);

    m_catalog.set_recipe(dough, make_recipe(1000, {{eggs, 2000}}));
    r = m_catalog.recipe(dough);
    ASSERT_TRUE(r);
    ASSERT_EQ(r->lines.size(), 1U) << "старые строки должны замениться, а не добавиться";
    EXPECT_THAT(r->lines[0].name, testing::StrEq("Яйцо"));

    m_catalog.remove_recipe(dough);
    EXPECT_FALSE(m_catalog.recipe(dough));
}

/**
 * @brief Проверки рецептуры: сырьё, нули, пустой состав, повтор, сам в себе, несуществующий компонент, предел.
 */
TEST_F(CatalogTest, RecipeValidation) {
    auto const flour = add("Мука", Kind::RAW);
    auto const dough = add("Тесто", Kind::SEMI);
    EXPECT_THROW_REASON(m_catalog.set_recipe(flour, make_recipe(1000, {{dough, 1}})), UserError, "у сырья нет рецептуры");
    EXPECT_THROW_REASON(m_catalog.set_recipe(dough, make_recipe(0, {{flour, 1}})), UserError, "выход: количество должно быть больше нуля");
    EXPECT_THROW_REASON(m_catalog.set_recipe(dough, make_recipe(1000, {})), UserError, "нет ни одного компонента");
    EXPECT_THROW_REASON(m_catalog.set_recipe(dough, make_recipe(1000, {{flour, -5}})), UserError, "Мука: количество должно быть больше нуля");
    EXPECT_THROW_REASON(m_catalog.set_recipe(dough, make_recipe(1000, {{flour, 1}, {flour, 2}})), UserError, "«Мука» указан в рецептуре дважды");
    EXPECT_THROW_REASON(m_catalog.set_recipe(dough, make_recipe(1000, {{dough, 1}})), UserError, "в собственную рецептуру");
    EXPECT_THROW_REASON(m_catalog.set_recipe(dough, make_recipe(1000, {{777, 1}})), NotFoundError, "id 777");
    EXPECT_THROW_REASON(m_catalog.set_recipe(dough, make_recipe(1000, {{flour, 1'000'000'001}})), UserError, "больше миллиона");
    EXPECT_FALSE(m_catalog.recipe(dough)) << "после отказов рецептура не должна появиться";
}

/**
 * @brief Цикл через несколько уровней отклоняется, в тексте весь путь цикла.
 */
TEST_F(CatalogTest, RecipeCycle) {
    auto const cake = add("Торт", Kind::PRODUCT, Unit::PCS);
    auto const cream = add("Крем", Kind::SEMI);
    auto const sponge = add("Бисквит", Kind::SEMI);
    m_catalog.set_recipe(cake, make_recipe(1000, {{cream, 300}}));
    m_catalog.set_recipe(cream, make_recipe(1000, {{sponge, 100}}));
    EXPECT_THROW_REASON(m_catalog.set_recipe(sponge, make_recipe(1000, {{cake, 1000}})), UserError,
                        "замкнётся в цикл: Бисквит → Торт → Крем → Бисквит");
}

/**
 * @brief Продукция может входить в рецептуру другой продукции (бисквит продаётся и идёт в торт).
 */
TEST_F(CatalogTest, ProductAsComponent) {
    auto const flour = add("Мука", Kind::RAW);
    auto const sponge = add("Бисквит", Kind::PRODUCT, Unit::KG);
    auto const cake = add("Торт", Kind::PRODUCT, Unit::PCS);
    m_catalog.set_recipe(sponge, make_recipe(1000, {{flour, 500}}));
    EXPECT_NO_THROW(m_catalog.set_recipe(cake, make_recipe(1000, {{sponge, 400}})));
}

/**
 * @brief Архивный компонент нельзя добавить, но можно оставить в рецептуре, где он уже был.
 */
TEST_F(CatalogTest, ArchivedComponent) {
    auto const flour = add("Мука", Kind::RAW);
    auto const raisin = add("Изюм", Kind::RAW);
    auto const bun = add("Булочка", Kind::PRODUCT, Unit::PCS);
    m_catalog.set_recipe(bun, make_recipe(1000, {{flour, 80}, {raisin, 10}}));

    auto r = m_catalog.get(raisin);
    r.archived = true;
    m_catalog.update(r);

    EXPECT_NO_THROW(m_catalog.set_recipe(bun, make_recipe(1000, {{flour, 90}, {raisin, 10}}))) << "изюм уже был в рецептуре булочки";

    auto const cake = add("Кекс", Kind::PRODUCT, Unit::PCS);
    EXPECT_THROW_REASON(m_catalog.set_recipe(cake, make_recipe(1000, {{raisin, 20}})), UserError, "«Изюм» в архиве");
}

/**
 * @brief Фотография: формат по содержимому, замена меняет etag, удаление позиции удаляет фото.
 */
TEST_F(CatalogTest, Photo) {
    auto const bread = add("Батон", Kind::PRODUCT, Unit::PCS);
    EXPECT_FALSE(m_catalog.photo(bread)) << "у новой позиции фото нет";
    EXPECT_FALSE(m_catalog.get(bread).photo);

    std::string const jpeg = std::string("\xFF\xD8\xFF\xE0", 4) + "jpeg-data";
    m_catalog.set_photo(bread, jpeg);
    auto photo = m_catalog.photo(bread);
    ASSERT_TRUE(photo) << "фото должно сохраниться";
    EXPECT_EQ(photo->mime, "image/jpeg");
    EXPECT_EQ(photo->data, jpeg) << "данные должны вернуться байт в байт";
    ASSERT_TRUE(m_catalog.get(bread).photo) << "позиция должна знать etag фото";
    EXPECT_EQ(*m_catalog.get(bread).photo, photo->etag);
    EXPECT_EQ(m_catalog.list({}).front().photo, photo->etag) << "etag есть и в списке";

    std::string const png = std::string("\x89PNG\r\n\x1A\n\0\0", 10);
    m_catalog.set_photo(bread, png);
    auto const replaced = m_catalog.photo(bread);
    ASSERT_TRUE(replaced);
    EXPECT_EQ(replaced->mime, "image/png") << "замена фото";
    EXPECT_EQ(replaced->data, png) << "нулевые байты не должны обрезаться";
    EXPECT_NE(replaced->etag, photo->etag) << "etag должен меняться вместе с фото";

    m_catalog.set_photo(bread, std::string("RIFF\x10\0\0\0WEBPVP8 ", 16));
    EXPECT_EQ(m_catalog.photo(bread)->mime, "image/webp");

    m_catalog.remove_photo(bread);
    EXPECT_FALSE(m_catalog.photo(bread)) << "фото удалено";
    EXPECT_NO_THROW(m_catalog.remove_photo(bread)) << "повторное удаление ничего не делает";

    m_catalog.set_photo(bread, jpeg);
    m_catalog.remove(bread);
    auto stmt = m_db.prepare("SELECT count(*) FROM item_photos");
    stmt.step();
    EXPECT_EQ(stmt.column_int(0), 0) << "фото удалённой позиции не должно оставаться в базе";
}

/**
 * @brief Пустой файл, не картинка, слишком большой файл и несуществующая позиция отклоняются с причиной.
 */
TEST_F(CatalogTest, PhotoRejected) {
    auto const bread = add("Батон", Kind::PRODUCT, Unit::PCS);
    EXPECT_THROW_REASON(m_catalog.set_photo(bread, ""), UserError, "пустой");
    EXPECT_THROW_REASON(m_catalog.set_photo(bread, "GIF89a"), UserError, "JPEG, PNG или WebP");
    EXPECT_THROW_REASON(m_catalog.set_photo(bread, "%PDF-1.7"), UserError, "«Батон»");
    std::string big(MAX_PHOTO_BYTES + 1, 'x');
    big.replace(0, 3, "\xFF\xD8\xFF");
    EXPECT_THROW_REASON(m_catalog.set_photo(bread, big), UserError, "не больше 5 МБ");
    EXPECT_FALSE(m_catalog.photo(bread)) << "после отказов фото нет";
    EXPECT_THROW(m_catalog.set_photo(9999, "\xFF\xD8\xFF"), NotFoundError);
    EXPECT_THROW((void)m_catalog.photo(9999), NotFoundError);
}
