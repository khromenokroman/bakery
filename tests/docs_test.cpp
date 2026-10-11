#include "docs.hpp"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "errors.hpp"
#include "fixture.hpp"
#include "test_util.hpp"

namespace {

class DocsTest : public DbFixture {
   protected:
    DocsTest() : m_bread{add("Батон", Kind::PRODUCT, Unit::PCS)}, m_pie{add("Пирог", Kind::PRODUCT, Unit::KG)} {}

    std::int64_t m_bread;
    std::int64_t m_pie;
};

} // namespace

/**
 * @brief Заявка сохраняется со строками, номера идут подряд.
 */
TEST_F(DocsTest, CreateAndNumbering) {
    auto const a = m_docs.create(make_order("2026-10-12", {{m_bread, 10000}, {m_pie, 2500}}, "Магазин на Ленина"));
    auto const b = m_docs.create(make_order("2026-10-12", {{m_bread, 5000}}));
    auto const doc = m_docs.get(a);
    EXPECT_EQ(doc.number, 1);
    EXPECT_EQ(m_docs.get(b).number, 2) << "номер следующей заявки";
    EXPECT_THAT(doc.comment, testing::StrEq("Магазин на Ленина"));
    EXPECT_EQ(doc.status, "draft");
    ASSERT_EQ(doc.lines.size(), 2U);
    EXPECT_THAT(doc.lines[1].name, testing::StrEq("Пирог"));
    EXPECT_EQ(doc.lines[1].unit, Unit::KG);
    EXPECT_EQ(doc.lines[1].qty, 2500);
}

/**
 * @brief Проверки заявки: дата, пустые строки, повтор, не продукция, архив, количество.
 */
TEST_F(DocsTest, Validation) {
    auto const dough = add("Тесто", Kind::SEMI);
    EXPECT_THROW_REASON(m_docs.create(make_order("12.10.2026", {{m_bread, 1000}})), UserError, "нужен формат ГГГГ-ММ-ДД");
    EXPECT_THROW_REASON(m_docs.create(make_order("2026-02-30", {{m_bread, 1000}})), UserError, "неверная дата «2026-02-30»");
    EXPECT_THROW_REASON(m_docs.create(make_order("2026-10-12", {})), UserError, "нет ни одной строки");
    EXPECT_THROW_REASON(m_docs.create(make_order("2026-10-12", {{m_bread, 1000}, {m_bread, 2000}})), UserError, "«Батон» указан в документе дважды");
    EXPECT_THROW_REASON(m_docs.create(make_order("2026-10-12", {{dough, 1000}})), UserError, "«Тесто» не продукция");
    EXPECT_THROW_REASON(m_docs.create(make_order("2026-10-12", {{m_bread, 0}})), UserError, "Батон: количество должно быть больше нуля");
    EXPECT_THROW_REASON(m_docs.create(make_order("2026-10-12", {{999, 1000}})), NotFoundError, "id 999");

    auto item = m_catalog.get(m_pie);
    item.archived = true;
    m_catalog.update(item);
    EXPECT_THROW_REASON(m_docs.create(make_order("2026-10-12", {{m_pie, 1000}})), UserError, "«Пирог» в архиве");
    EXPECT_TRUE(m_docs.list({}).empty()) << "после отказов документов быть не должно";
}

/**
 * @brief Изменение заменяет строки; архивная позиция, уже бывшая в заявке, не мешает сохранению.
 */
TEST_F(DocsTest, Update) {
    auto const id = m_docs.create(make_order("2026-10-12", {{m_bread, 10000}, {m_pie, 1000}}));
    auto item = m_catalog.get(m_pie);
    item.archived = true;
    m_catalog.update(item);

    auto doc = make_order("2026-10-13", {{m_pie, 3000}}, "перенесли");
    doc.id = id;
    m_docs.update(doc);
    auto const saved = m_docs.get(id);
    EXPECT_EQ(saved.date, "2026-10-13");
    EXPECT_THAT(saved.comment, testing::StrEq("перенесли"));
    ASSERT_EQ(saved.lines.size(), 1U) << "строки должны замениться";
    EXPECT_EQ(saved.lines[0].qty, 3000);
    EXPECT_EQ(saved.number, 1) << "номер при изменении не меняется";
}

/**
 * @brief Позицию из заявки нельзя удалить из справочника, после удаления заявки можно.
 */
TEST_F(DocsTest, RemoveAndItemUsage) {
    auto const id = m_docs.create(make_order("2026-10-12", {{m_pie, 1000}}));
    EXPECT_THROW_REASON(m_catalog.remove(m_pie), UserError, "есть в документах");
    m_docs.remove(id);
    EXPECT_THROW_REASON((void)m_docs.get(id), NotFoundError, "нет документа");
    EXPECT_NO_THROW(m_catalog.remove(m_pie));
}

/**
 * @brief Фильтр по виду и датам включительно, сортировка по дате.
 */
TEST_F(DocsTest, ListFilter) {
    m_docs.create(make_order("2026-10-13", {{m_bread, 1000}}, "c"));
    m_docs.create(make_order("2026-10-11", {{m_bread, 1000}}, "a"));
    m_docs.create(make_order("2026-10-12", {{m_bread, 1000}}, "b"));
    DocFilter filter;
    filter.types = {DocType::ORDER};
    filter.date_from = "2026-10-11";
    filter.date_to = "2026-10-12";
    std::vector<std::string> comments;
    for (auto const &d : m_docs.list(filter)) {
        comments.push_back(d.comment);
    }
    EXPECT_THAT(comments, testing::ElementsAre("a", "b")) << "с 11 по 12 включительно, по дате";
}
