#include "period.hpp"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <optional>
#include <string>

#include "docs.hpp"
#include "errors.hpp"
#include "fixture.hpp"
#include "prices.hpp"
#include "repost.hpp"
#include "settings.hpp"
#include "stock.hpp"
#include "test_util.hpp"

namespace {

/**
 * @brief Батон: поступление 30.09 08:00 на 10 шт (проведено), черновик списания 30.09 12:00 на 1 шт,
 * заявка на 30.09 и поступление 01.10 08:00 на 5 шт (проведено).
 */
class PeriodTest : public DbFixture {
   protected:
    PeriodTest() : m_bread{add("Батон", Kind::PRODUCT, Unit::PCS)} {
        m_sep = create(DocType::PRODUCTION, "2026-09-30", "08:00", 10000);
        m_docs.post(m_sep);
        m_draft = create(DocType::WRITEOFF, "2026-09-30", "12:00", 1000, "брак");
        m_order = create(DocType::ORDER, "2026-09-30", "00:00", 3000);
        m_oct = create(DocType::PRODUCTION, "2026-10-01", "08:00", 5000);
        m_docs.post(m_oct);
    }

    std::int64_t create(DocType type, std::string const &date, std::string const &time, std::int64_t qty, std::string comment = {}) {
        return m_docs.create(doc(type, date, time, qty, std::move(comment)));
    }

    Document doc(DocType type, std::string const &date, std::string const &time, std::int64_t qty, std::string comment = {}) const {
        auto d = make_doc(type, date, {{m_bread, qty}}, std::move(comment));
        d.time = time;
        return d;
    }

    /// Документ id с другим количеством и моментом.
    Document changed(std::int64_t id, std::int64_t qty, std::optional<std::string> date = std::nullopt) const {
        auto d = m_docs.get(id);
        d.lines.at(0).qty = qty;
        if (date) {
            d.date = *date;
        }
        return d;
    }

    std::int64_t bread_stock() {
        auto const rows = Stock{m_db}.balances(std::nullopt);
        return rows.empty() ? 0 : rows.at(0).qty;
    }

    std::int64_t m_bread;
    std::int64_t m_sep{};
    std::int64_t m_draft{};
    std::int64_t m_order{};
    std::int64_t m_oct{};
};

} // namespace

/**
 * @brief Без даты закрытия ничего не закрыто, дата хранится и снимается.
 */
TEST_F(PeriodTest, CloseAndOpen) {
    Period period{m_db};
    EXPECT_EQ(period.closed_till(), std::nullopt) << "в новой базе период не закрыт";
    EXPECT_FALSE(period.is_closed("2000-01-01 00:00:00"));

    period.close("2026-09-30");
    EXPECT_EQ(period.closed_till(), "2026-09-30");
    EXPECT_TRUE(period.is_closed("2026-09-30 23:59:59")) << "дата закрытия включительно";
    EXPECT_FALSE(period.is_closed("2026-10-01 00:00:00"));

    period.close("2026-09-15");
    EXPECT_EQ(period.closed_till(), "2026-09-15") << "сдвинуть дату назад (открыть часть периода) можно";
    period.close(std::nullopt);
    EXPECT_EQ(period.closed_till(), std::nullopt) << "nullopt открывает все периоды";

    EXPECT_THROW_REASON(period.close("2026-02-30"), UserError, "2026-02-30");
    EXPECT_THROW_REASON(period.close("30.09.2026"), UserError, "30.09.2026");
}

/**
 * @brief В закрытом периоде документы любого вида нельзя создать, изменить, удалить, провести,
 * отменить и перепровести; остатки не меняются. Документы после даты закрытия работают как обычно.
 */
TEST_F(PeriodTest, ClosedDocumentsAreFrozen) {
    Period{m_db}.close("2026-09-30");
    auto const before = bread_stock();
    ASSERT_EQ(before, 15000);

    EXPECT_THROW_REASON(create(DocType::PRODUCTION, "2026-09-30", "23:59", 1000), UserError,
                        "период по 30.09.2026 закрыт: создать документ от 30.09.2026 23:59 нельзя");
    EXPECT_THROW_REASON(create(DocType::ORDER, "2026-09-01", "00:00", 1000), UserError, "создать документ от 01.09.2026 нельзя");
    auto prices = make_doc(DocType::PRICES, "2026-09-10", {});
    prices.price_type_id = COST_PRICE_TYPE;
    prices.lines.push_back(DocLine{.price_kop = 3000, .item_id = m_bread});
    EXPECT_THROW_REASON(m_docs.create(prices), UserError, "создать документ от 10.09.2026 нельзя");

    EXPECT_THROW_REASON(m_docs.update(changed(m_sep, 20000)), UserError,
                        "изменить документ «Поступление из производства № 1 от 30.09.2026 08:00» нельзя");
    EXPECT_THROW_REASON(m_docs.update(changed(m_order, 1000)), UserError, "изменить документ «Заявка № 1 от 30.09.2026» нельзя");
    EXPECT_THROW_REASON(m_docs.update(changed(m_oct, 5000, "2026-09-30")), UserError, "перенести документ на 30.09.2026 08:00 нельзя");
    EXPECT_THROW_REASON(m_docs.remove(m_draft), UserError, "удалить документ «Списание № 1 от 30.09.2026 12:00» нельзя");
    EXPECT_THROW_REASON(m_docs.remove(m_order), UserError, "удалить документ «Заявка № 1 от 30.09.2026» нельзя");
    EXPECT_THROW_REASON(m_docs.post(m_draft), UserError, "провести документ «Списание № 1 от 30.09.2026 12:00» нельзя");
    EXPECT_THROW_REASON(m_docs.unpost(m_sep), UserError,
                        "отменить проведение документа «Поступление из производства № 1 от 30.09.2026 08:00» нельзя");
    EXPECT_THROW_REASON(m_docs.repost(m_sep), UserError, "перепровести документ «Поступление из производства № 1 от 30.09.2026 08:00» нельзя");

    EXPECT_EQ(bread_stock(), before) << "отказы не должны менять остаток";
    EXPECT_EQ(m_docs.get(m_sep).lines.at(0).qty, 10000);
    EXPECT_EQ(m_docs.get(m_draft).status, "draft");

    // после даты закрытия всё работает
    m_docs.update(changed(m_oct, 6000));
    auto const fresh = create(DocType::WRITEOFF, "2026-10-01", "00:00", 1000, "бой");
    m_docs.post(fresh);
    m_docs.unpost(fresh);
    m_docs.remove(fresh);
    EXPECT_EQ(bread_stock(), 16000) << "10 в закрытом периоде + 6 поступлением 01.10";

    Period{m_db}.close(std::nullopt);
    m_docs.post(m_draft);
    EXPECT_EQ(bread_stock(), 15000) << "после открытия периода документ проводится";
}

/**
 * @brief Закрыть период нельзя, пока документы в нём ждут перепроведения; перепроводить с момента
 * в закрытом периоде нельзя.
 */
TEST_F(PeriodTest, PendingRepostBlocksClosing) {
    auto const inv = create(DocType::INVENTORY, "2026-10-01", "20:00", 14000);
    m_docs.post(inv);
    m_docs.update(changed(m_sep, 12000)); // задним числом: инвентаризация 01.10 ждёт перепроведения
    Reposter reposter{m_db};
    auto const state = reposter.state();
    ASSERT_TRUE(state.from.has_value()) << "отметка о перепроведении должна быть поставлена";
    ASSERT_EQ(state.from->moment, "2026-09-30 08:00:00");

    Period period{m_db};
    EXPECT_THROW_REASON(period.close("2026-09-30"), UserError,
                        "нельзя закрыть период по 30.09.2026: документы с 30.09.2026 08:00 ждут перепроведения");
    EXPECT_THROW_REASON(period.close("2026-10-05"), UserError, "ждут перепроведения");
    EXPECT_EQ(period.closed_till(), std::nullopt) << "при отказе дата не меняется";
    period.close("2026-09-29");
    EXPECT_EQ(period.closed_till(), "2026-09-29") << "до отметки закрыть можно";

    EXPECT_THROW_REASON((void)reposter.count("2026-09-29 12:00:00"), UserError, "перепроводить документы с 29.09.2026 12:00 нельзя");
    EXPECT_THROW_REASON(reposter.step("2026-09-01 00:00:00"), UserError, "период по 29.09.2026 закрыт");

    for (int i = 0; i < 10; ++i) {
        auto const step = reposter.step("2026-09-30 00:00:00");
        ASSERT_FALSE(step.error.has_value()) << *step.error;
        if (!step.doc) {
            break;
        }
    }
    EXPECT_FALSE(reposter.state().from.has_value()) << "перепроведение дошло до конца, отметка снята";
    period.close("2026-10-05");
    EXPECT_EQ(period.closed_till(), "2026-10-05") << "после перепроведения период закрывается";

    period.close(std::nullopt);
    m_docs.update(changed(m_sep, 11000));
    period.close("2026-09-29");
    EXPECT_EQ(period.closed_till(), "2026-09-29") << "сдвиг назад при отметке не запрещён";
}

/**
 * @brief Дата закрытия не видна и не меняется через реквизиты организации.
 */
TEST_F(PeriodTest, NotASetting) {
    Period{m_db}.close("2026-09-30");
    Settings settings{m_db};
    EXPECT_FALSE(settings.all().contains("closed_till")) << "дата закрытия не реквизит";
    EXPECT_THROW_REASON(settings.update({{"closed_till", "2026-10-31"}}), UserError, "неизвестная настройка: closed_till");
    settings.update({{"org_name", "Пекарня"}});
    EXPECT_EQ(Period{m_db}.closed_till(), "2026-09-30") << "сохранение реквизитов не трогает дату закрытия";
}
