#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "docs.hpp"
#include "errors.hpp"
#include "fixture.hpp"
#include "prices.hpp"
#include "report.hpp"
#include "repost.hpp"
#include "stock.hpp"
#include "test_util.hpp"

namespace {

/**
 * @brief Строка для построения документа: позиция, количество в тысячных, цена в копейках.
 */
struct Line {
    std::int64_t item{};
    std::int64_t qty{};
    std::optional<std::int64_t> price;
};

/**
 * @brief Ожидаемое состояние склада после шага: количества и оценка остатка.
 */
struct State {
    std::int64_t bread{};  ///< батон, тысячные штуки
    std::int64_t cookie{}; ///< печенье, граммы
    std::int64_t cost{};   ///< остаток по себестоимости, копейки
    std::int64_t retail{}; ///< остаток по «Рознице», копейки
};

/**
 * @brief Полный цикл документов по двум позициям: батон (шт) и печенье (кг), типы цен
 * «Себестоимость» и «Розница». Шаги идут по времени, у каждого шага - ожидаемое состояние.
 *
 * | шаг | момент      | документ                                        | батон | печенье | себест. | розница |
 * |-----|-------------|-------------------------------------------------|-------|---------|---------|---------|
 * |  1  | 01.10 09:00 | себестоимость: батон 10, печенье 200;           |   0   |  0      |    0    |    0    |
 * |     |             | розница: батон 25, печенье 400                  |       |         |         |         |
 * |  2  | 02.10 08:00 | поступление: батон 100, печенье 5               |  100  |  5      | 2000    | 4500    |
 * |  3  | 02.10 12:00 | реализация (розница): батон 60, печенье 2       |   40  |  3      | 1000    | 2200    |
 * |  4  | 03.10 10:00 | списание: батон 5 (брак)                        |   35  |  3      |  950    | 2075    |
 * |  5  | 04.10 09:00 | себестоимость батона 12 (рост, +70)             |   35  |  3      | 1020    | 2075    |
 * |  6  | 04.10 09:30 | розница батона 30 (рост, +175)                  |   35  |  3      | 1020    | 2250    |
 * |  7  | 05.10 11:00 | списание: печенье 0,5 (бой)                     |   35  |  2,5    |  920    | 2050    |
 * |  8  | 06.10 09:00 | себестоимость печенья 150 (снижение, -125)      |   35  |  2,5    |  795    | 2050    |
 * |  9  | 07.10 12:00 | реализация (розница): батон 20 по 30            |   15  |  2,5    |  555    | 1450    |
 * | 10  | 08.10 20:00 | инвентаризация: батон 14 (-1), печенье 2,6 (+0,1)|  14  |  2,6    |  558    | 1460    |
 * | 11  | 09.10 10:00 | возврат по шагу 3: батон 10                     |   24  |  2,6    |  678    | 1760    |
 * | 12  | 10.10 08:00 | поступление: батон 50                           |   74  |  2,6    | 1278    | 3260    |
 * | 13  | 11.10 12:00 | реализация: батон 10 по 20 (розница 30, -100)    |   64  |  2,6    | 1158    | 2960    |
 * | 14  | 11.10 15:00 | реализация: батон 4 по 35 (+20), печенье 0,6     |   60  |  2,0    | 1020    | 2600    |
 * |     |             | по 375,50 (розница 400, -14,70)                 |       |         |         |         |
 *
 * Продавец вправе менять цену в реализации: шаги 13 и 14 продают не по рознице. Остаток по рознице всё
 * равно количество × розничная цена, а в отчёте разница идёт переоценкой при продаже.
 */
class CycleTest : public DbFixture {
   protected:
    CycleTest()
        : m_bread{add("Батон", Kind::PRODUCT, Unit::PCS)},
          m_cookie{add("Печенье", Kind::PRODUCT, Unit::KG)},
          m_retail{Prices{m_db}.create_type("Розница")} {}

    /// Документ вида type на момент date time.
    Document make(DocType type, std::string const &date, std::string const &time, std::vector<Line> const &lines, std::string comment = {}) const {
        Document d;
        d.type = type;
        d.date = date;
        d.time = time;
        d.comment = std::move(comment);
        for (auto const &l : lines) {
            DocLine dl;
            dl.item_id = l.item;
            dl.qty = l.qty;
            dl.price_kop = l.price;
            d.lines.push_back(dl);
        }
        if (type == DocType::SALE) {
            d.price_type_id = m_retail;
            d.partner_id = m_buyer;
        }
        return d;
    }

    /// Создаёт и проводит документ.
    std::int64_t post(Document const &d) {
        auto const id = m_docs.create(d);
        m_docs.post(id);
        return id;
    }

    /// Установка цен типа type.
    std::int64_t set_prices(std::int64_t type, std::string const &date, std::string const &time, std::vector<Line> const &lines) {
        auto d = make(DocType::PRICES, date, time, lines);
        d.price_type_id = type;
        return post(d);
    }

    /// Шаги цикла по порядку; индекс + 1 - номер шага из таблицы.
    std::vector<std::function<void()>> steps() {
        return {
            [this] {
                set_prices(COST_PRICE_TYPE, "2026-10-01", "09:00", {{m_bread, 0, 1000}, {m_cookie, 0, 20000}});
                set_prices(m_retail, "2026-10-01", "09:00", {{m_bread, 0, 2500}, {m_cookie, 0, 40000}});
            },
            [this] {
                m_production = post(make(DocType::PRODUCTION, "2026-10-02", "08:00", {{m_bread, 100000}, {m_cookie, 5000}}));
            },
            [this] {
                m_sale = post(make(DocType::SALE, "2026-10-02", "12:00", {{m_bread, 60000, 2500}, {m_cookie, 2000, 40000}}));
            },
            [this] {
                post(make(DocType::WRITEOFF, "2026-10-03", "10:00", {{m_bread, 5000}}, "брак"));
            },
            [this] {
                set_prices(COST_PRICE_TYPE, "2026-10-04", "09:00", {{m_bread, 0, 1200}});
            },
            [this] {
                set_prices(m_retail, "2026-10-04", "09:30", {{m_bread, 0, 3000}});
            },
            [this] {
                post(make(DocType::WRITEOFF, "2026-10-05", "11:00", {{m_cookie, 500}}, "бой"));
            },
            [this] {
                set_prices(COST_PRICE_TYPE, "2026-10-06", "09:00", {{m_cookie, 0, 15000}});
            },
            [this] {
                m_sale2 = post(make(DocType::SALE, "2026-10-07", "12:00", {{m_bread, 20000, 3000}}));
            },
            [this] {
                m_inventory = post(make(DocType::INVENTORY, "2026-10-08", "20:00", {{m_bread, 14000}, {m_cookie, 2600}}));
            },
            [this] {
                auto ret = make(DocType::RETURN, "2026-10-09", "10:00", {{m_bread, 10000}});
                ret.base_id = m_sale;
                m_return = post(ret);
            },
            [this] {
                m_production2 = post(make(DocType::PRODUCTION, "2026-10-10", "08:00", {{m_bread, 50000}}));
            },
            [this] {
                m_cheaper = post(make(DocType::SALE, "2026-10-11", "12:00", {{m_bread, 10000, 2000}}));
            },
            [this] {
                m_mixed = post(make(DocType::SALE, "2026-10-11", "15:00", {{m_bread, 4000, 3500}, {m_cookie, 600, 37550}}));
            },
        };
    }

    /// Ожидаемое состояние после каждого шага, по таблице.
    static std::vector<State> expected() {
        return {
            {0, 0, 0, 0},
            {100000, 5000, 200000, 450000},
            {40000, 3000, 100000, 220000},
            {35000, 3000, 95000, 207500},
            {35000, 3000, 102000, 207500},
            {35000, 3000, 102000, 225000},
            {35000, 2500, 92000, 205000},
            {35000, 2500, 79500, 205000},
            {15000, 2500, 55500, 145000},
            {14000, 2600, 55800, 146000},
            {24000, 2600, 67800, 176000},
            {74000, 2600, 127800, 326000},
            {64000, 2600, 115800, 296000},
            {60000, 2000, 102000, 260000},
        };
    }

    /// Последний день цикла: на его конец проверяется итоговое состояние.
    static constexpr char const *LAST_DAY = "2026-10-11";

    /// Проводит весь цикл.
    void run_cycle() {
        for (auto const &step : steps()) {
            step();
        }
    }

    /// Остаток позиции на конец дня date, 0 если остатка нет.
    std::int64_t qty(std::int64_t item, std::string const &date) {
        for (auto const &b : Stock{m_db}.balances(date)) {
            if (b.item_id == item) {
                return b.qty;
            }
        }
        return 0;
    }

    /**
     * @brief Сверяет склад на конец дня date с ожиданием тремя путями: остатки склада, остаток × цена
     * на конец дня и товарный отчёт с начала цикла (по себестоимости и по «Рознице»).
     */
    void expect_state(std::string const &date, State const &want) {
        EXPECT_EQ(qty(m_bread, date), want.bread) << "остаток батона на конец " << date;
        EXPECT_EQ(qty(m_cookie, date), want.cookie) << "остаток печенья на конец " << date;

        Prices const prices{m_db};
        auto const value = [&](std::int64_t type) {
            std::int64_t sum{};
            for (auto const item : {m_bread, m_cookie}) {
                sum += qty(item, date) * prices.price(type, item, date + " 23:59:59").value_or(0);
            }
            return sum / 1000;
        };
        EXPECT_EQ(value(COST_PRICE_TYPE), want.cost) << "остаток × себестоимость на конец " << date;
        EXPECT_EQ(value(m_retail), want.retail) << "остаток × розничная цена на конец " << date;

        auto const r = goods_report(m_db, "2026-10-01", date, std::nullopt, m_retail);
        EXPECT_EQ(r.opening_sum, 0) << "до цикла склад пуст";
        EXPECT_EQ(r.closing_sum, want.cost) << "товарный отчёт 01.10-" << date << ": остаток на конец по себестоимости";
        EXPECT_EQ(r.closing_price_sum, want.retail) << "товарный отчёт 01.10-" << date << ": остаток на конец по рознице";
        EXPECT_EQ(r.closing_sum, r.opening_sum + r.in_sum - r.out_sum) << "конец = начало + приход - расход";
        EXPECT_EQ(r.closing_price_sum, r.opening_price_sum + r.in_price_sum - r.out_price_sum) << "то же по рознице";
        EXPECT_EQ(r.lines_without_cost, 0) << "себестоимость поставлена до движений";
        EXPECT_EQ(r.moves_without_price, 0) << "розница поставлена до движений";
        for (auto const &item : r.items) {
            EXPECT_EQ(item.closing, item.opening + item.in - item.out) << item.name << ": количество в отчёте";
            EXPECT_EQ(item.closing, qty(item.item_id, date)) << item.name << ": отчёт и склад";
            EXPECT_EQ(item.closing_sum, item.closing * prices.price(COST_PRICE_TYPE, item.item_id, date + " 23:59:59").value_or(0) / 1000)
                << item.name << ": стоимость остатка по позиции = количество × себестоимость, с переоценками";
        }
    }

    /// Себестоимость и цена строки позиции item в документе id.
    std::pair<std::optional<std::int64_t>, std::optional<std::int64_t>> line(std::int64_t id, std::int64_t item) const {
        for (auto const &l : m_docs.get(id).lines) {
            if (l.item_id == item) {
                return {l.cost_kop, l.price_kop};
            }
        }
        ADD_FAILURE() << "в документе " << id << " нет позиции " << item;
        return {};
    }

    /**
     * @brief Перепроводит документы с момента from до конца или до ошибки, как веб-интерфейс: по шагу,
     * место прогона хранит сервер. Каждый раз новый Reposter - как после перезагрузки страницы.
     * @return последний шаг: без документа - дошли до конца, с ошибкой - остановились
     */
    RepostStep repost_all(std::string const &from, bool restart = false) {
        m_reposted.clear();
        for (;;) {
            auto step = Reposter{m_db}.step(from, restart);
            restart = false;
            if (!step.doc || step.error) {
                return step;
            }
            m_reposted.push_back(step.doc->id);
        }
    }

    /// Нужно ли перепроведение.
    bool needed() { return Reposter{m_db}.state().from.has_value(); }

    std::vector<std::int64_t> m_reposted; ///< документы, перепроведённые последним repost_all
    std::int64_t m_bread;
    std::int64_t m_cookie;
    std::int64_t m_retail;
    std::int64_t m_production{};
    std::int64_t m_sale{};
    std::int64_t m_sale2{};
    std::int64_t m_inventory{};
    std::int64_t m_return{};
    std::int64_t m_production2{};
    std::int64_t m_cheaper{}; ///< реализация дешевле розницы
    std::int64_t m_mixed{};   ///< реализация: батон дороже розницы, печенье дешевле
};

} // namespace

/**
 * @brief Шаг за шагом: после каждого документа остатки, оценка остатка по себестоимости и рознице и
 * товарный отчёт с начала цикла совпадают с таблицей; отчёт по каждому дню начинается с остатка
 * на конец предыдущего.
 */
TEST_F(CycleTest, StepByStep) {
    auto const all = steps();
    auto const want = expected();
    ASSERT_EQ(all.size(), want.size());
    std::string prev_day;
    for (std::size_t i = 0; i < all.size(); ++i) {
        SCOPED_TRACE("шаг " + std::to_string(i + 1));
        ASSERT_NO_THROW(all[i]());
        // шаги 1-14 приходятся на 01.10-11.10, по два шага на 02.10, 04.10 и 11.10
        static constexpr char const *DAYS[] = {"2026-10-01", "2026-10-02", "2026-10-02", "2026-10-03", "2026-10-04", "2026-10-04", "2026-10-05",
                                               "2026-10-06", "2026-10-07", "2026-10-08", "2026-10-09", "2026-10-10", "2026-10-11", "2026-10-11"};
        static_assert(std::size(DAYS) == 14);
        std::string const day = DAYS[i];
        expect_state(day, want[i]);
        if (!prev_day.empty() && prev_day != day) {
            auto const before = goods_report(m_db, "2026-10-01", prev_day, std::nullopt, m_retail);
            auto const today = goods_report(m_db, day, day, std::nullopt, m_retail);
            EXPECT_EQ(today.opening_sum, before.closing_sum) << "начало " << day << " = конец " << prev_day;
            EXPECT_EQ(today.opening_price_sum, before.closing_price_sum) << "то же по рознице";
        }
        prev_day = day;
    }
}

/**
 * @brief Себестоимость и цена в строках документов: себестоимость - на момент документа, цена
 * возврата - из реализации, себестоимость возврата - на момент возврата.
 */
TEST_F(CycleTest, LineCosts) {
    run_cycle();
    EXPECT_EQ(line(m_production, m_bread).first, 1000) << "поступление 02.10: батон по 10,00";
    EXPECT_EQ(line(m_sale, m_bread), std::pair(std::optional<std::int64_t>{1000}, std::optional<std::int64_t>{2500}))
        << "реализация 02.10: себестоимость 10,00, цена 25,00";
    EXPECT_EQ(line(m_sale, m_cookie).first, 20000);
    EXPECT_EQ(line(m_sale2, m_bread).first, 1200) << "реализация 07.10 после роста себестоимости";
    EXPECT_EQ(line(m_inventory, m_bread).first, 1200);
    EXPECT_EQ(line(m_inventory, m_cookie).first, 15000) << "инвентаризация 08.10 после снижения себестоимости печенья";
    EXPECT_EQ(line(m_return, m_bread), std::pair(std::optional<std::int64_t>{1200}, std::optional<std::int64_t>{2500}))
        << "возврат: цена 25,00 из реализации, себестоимость 12,00 на 09.10";
    EXPECT_EQ(line(m_production2, m_bread).first, 1200);
}

/**
 * @brief Товарный отчёт за весь цикл: документы прихода и расхода по времени с суммами по
 * себестоимости и рознице.
 *
 * Приход по себестоимости 2 805,00: поступление 2 000, переоценка батона 70, излишек печенья 15,
 * возврат 120, поступление 600. Расход 1 785,00: реализация 1 000, списания 50 и 100, переоценка
 * печенья 125, реализация 240, недостача батона 12, реализации 120 и 138. Конец 1 020,00.
 *
 * По рознице реализация списывается по фактической цене, разница с розницей - переоценка при продаже:
 * шаг 13 дешевле (расход 100), шаг 14 батон дороже (приход 20) и печенье дешевле (расход 14,70).
 * Конец по рознице 2 600,00 = 60 × 30 + 2 × 400.
 */
TEST_F(CycleTest, Report) {
    run_cycle();
    auto const r = goods_report(m_db, "2026-10-01", LAST_DAY, std::nullopt, m_retail);
    EXPECT_EQ(r.in_sum, 280500);
    EXPECT_EQ(r.out_sum, 178500);
    EXPECT_EQ(r.closing_sum, 102000);
    EXPECT_EQ(r.in_price_sum, 653500);
    EXPECT_EQ(r.out_price_sum, 393500);
    EXPECT_EQ(r.closing_price_sum, 260000);

    struct Row {
        DocType type;
        std::int64_t sum;
        std::int64_t price_sum;
        bool revaluation = false;
    };
    auto const check = [](std::vector<ReportDoc> const &got, std::vector<Row> const &want, char const *what) {
        ASSERT_EQ(got.size(), want.size()) << what;
        for (std::size_t i = 0; i < want.size(); ++i) {
            SCOPED_TRACE(std::string(what) + ", строка " + std::to_string(i + 1) + " (" + got[i].date + " " + got[i].time + ")");
            EXPECT_EQ(got[i].type, want[i].type);
            EXPECT_EQ(got[i].revaluation, want[i].revaluation) << "переоценка при продаже";
            EXPECT_EQ(got[i].sum, want[i].sum) << "по себестоимости";
            EXPECT_EQ(got[i].price_sum, want[i].price_sum) << "по рознице";
        }
    };
    check(r.incoming,
          {
              {DocType::PRODUCTION, 200000, 450000},
              {DocType::PRICES, 7000, 0},       // себестоимость батона 10 -> 12, 35 шт
              {DocType::PRICES, 0, 17500},      // розница батона 25 -> 30, 35 шт
              {DocType::INVENTORY, 1500, 4000}, // излишек печенья 0,1 кг
              {DocType::RETURN, 12000, 30000},  // 10 шт: по рознице на момент возврата
              {DocType::PRODUCTION, 60000, 150000},
              {DocType::SALE, 0, 2000, true}, // батон 4 шт продан по 35 при рознице 30
          },
          "приход");
    check(r.outgoing,
          {
              {DocType::SALE, 100000, 230000},
              {DocType::WRITEOFF, 5000, 12500},
              {DocType::WRITEOFF, 10000, 20000},
              {DocType::PRICES, 12500, 0}, // себестоимость печенья 200 -> 150, 2,5 кг
              {DocType::SALE, 24000, 60000},
              {DocType::INVENTORY, 1200, 3000}, // недостача батона 1 шт
              {DocType::SALE, 12000, 20000},    // 10 шт по фактической цене 20
              {DocType::SALE, 0, 10000, true},  // переоценка: розница 30, продано по 20
              {DocType::SALE, 13800, 36530},    // батон 4 × 35 + печенье 0,6 × 375,50
              {DocType::SALE, 0, 1470, true},   // переоценка печенья: 0,6 × (400 - 375,50)
          },
          "расход");

    auto const plain = goods_report(m_db, "2026-10-01", LAST_DAY);
    EXPECT_EQ(plain.incoming.size(), 5U) << "без типа цен переоценки розницы нет";
    EXPECT_EQ(plain.closing_sum, 102000);

    auto const tail = goods_report(m_db, "2026-10-04", "2026-10-07", std::nullopt, m_retail);
    EXPECT_EQ(tail.opening_sum, 95000) << "конец 03.10";
    EXPECT_EQ(tail.opening_price_sum, 207500);
    EXPECT_EQ(tail.closing_sum, 55500) << "конец 07.10";
    EXPECT_EQ(tail.closing_price_sum, 145000);
}

/**
 * @brief Запрещённые действия в середине цикла отклоняются с понятной причиной и ничего не меняют.
 */
TEST_F(CycleTest, Rejections) {
    run_cycle();

    // на 07.10 13:00 батона 15 шт, позже инвентаризация и поступление - но минус в прошлом запрещён
    EXPECT_THROW_REASON(post(make(DocType::SALE, "2026-10-07", "13:00", {{m_bread, 16000, 3000}})), UserError, "уйдёт в минус");
    EXPECT_THROW_REASON(post(make(DocType::WRITEOFF, "2026-10-03", "10:30", {{m_bread, 36000}}, "брак")), UserError, "уйдёт в минус");
    EXPECT_THROW_REASON(post(make(DocType::WRITEOFF, "2026-10-10", "09:00", {{m_bread, 1000}}, " ")), UserError, "укажите причину списания");

    auto ret = make(DocType::RETURN, "2026-10-10", "09:00", {{m_bread, 51000}});
    ret.base_id = m_sale;
    // продано 60, уже возвращено 10
    EXPECT_THROW_REASON(post(ret), UserError, "можно вернуть не больше 50 шт");
    ret = make(DocType::RETURN, "2026-10-02", "11:00", {{m_bread, 1000}});
    ret.base_id = m_sale;
    EXPECT_THROW_REASON(post(ret), UserError, "возврат не может быть раньше реализации");

    auto no_price = make(DocType::SALE, "2026-10-10", "09:00", {{m_bread, 1000}});
    no_price.price_type_id = Prices{m_db}.create_type("Опт");
    EXPECT_THROW_REASON(post(no_price), UserError, "нет цены в типе «Опт»");

    EXPECT_THROW_REASON(m_docs.unpost(m_sale), UserError, "по реализации проведены возвраты");
    EXPECT_THROW_REASON(m_docs.unpost(m_production), UserError, "уйдёт в минус");
    EXPECT_THROW_REASON(m_docs.remove(m_production), UserError, "документ проведён, удалить его нельзя");

    // уменьшение проведённого поступления задним числом: к реализации 07.10 батона не хватит
    auto smaller = m_docs.get(m_production);
    smaller.lines[0].qty = 80000;
    EXPECT_THROW_REASON(m_docs.update(smaller), UserError, "уйдёт в минус");
    EXPECT_EQ(m_docs.get(m_production).lines[0].qty, 100000) << "при отказе документ остаётся прежним";

    expect_state(LAST_DAY, expected().back());
}

/**
 * @brief Отмена проведения с конца и повторное проведение возвращают те же остатки и тот же отчёт.
 */
TEST_F(CycleTest, UnpostAndRepost) {
    run_cycle();
    auto const want = expected();
    auto const report_before = goods_report(m_db, "2026-10-01", LAST_DAY, std::nullopt, m_retail);

    m_docs.unpost(m_mixed);
    expect_state(LAST_DAY, want[12]);
    m_docs.unpost(m_cheaper);
    expect_state(LAST_DAY, want[11]);
    m_docs.unpost(m_production2);
    expect_state(LAST_DAY, want[10]);
    m_docs.unpost(m_return);
    expect_state(LAST_DAY, want[9]);
    m_docs.unpost(m_inventory);
    expect_state(LAST_DAY, want[8]);
    m_docs.unpost(m_sale2);
    expect_state(LAST_DAY, want[7]);
    {
        SCOPED_TRACE("реализацию можно отменить, когда её возврат отменён");
        // теперь 35 батонов и 2,5 кг печенья по ценам 06.10: снимаем и первую реализацию
        ASSERT_NO_THROW(m_docs.unpost(m_sale));
        EXPECT_EQ(qty(m_bread, LAST_DAY), 95000);
        ASSERT_NO_THROW(m_docs.post(m_sale));
    }

    for (auto const id : {m_sale2, m_inventory, m_return, m_production2, m_cheaper, m_mixed}) {
        ASSERT_NO_THROW(m_docs.post(id)) << "документ " << id;
    }
    expect_state(LAST_DAY, want.back());
    auto const report_after = goods_report(m_db, "2026-10-01", LAST_DAY, std::nullopt, m_retail);
    EXPECT_EQ(report_after.in_sum, report_before.in_sum);
    EXPECT_EQ(report_after.out_sum, report_before.out_sum);
    EXPECT_EQ(report_after.in_price_sum, report_before.in_price_sum);
    EXPECT_EQ(report_after.out_price_sum, report_before.out_price_sum);
}

/**
 * @brief Документы, проведённые по порядку времени, перепроведения не требуют.
 */
TEST_F(CycleTest, InOrderNoRepost) {
    run_cycle();
    EXPECT_FALSE(needed()) << "цикл проведён по порядку, отметки быть не должно";
}

/**
 * @brief Поступление 02.10 увеличено на 10 батонов задним числом: инвентаризация 08.10 (насчитано 14)
 * устарела, после неё 24 шт. Отметка указывает на поступление; перепроведение всех документов с 02.10
 * возвращает насчитанное и снимает отметку.
 */
TEST_F(CycleTest, RepostAfterBackdatedChange) {
    run_cycle();
    auto bigger = m_docs.get(m_production);
    bigger.lines[0].qty = 110000;
    ASSERT_NO_THROW(m_docs.update(bigger));
    EXPECT_EQ(qty(m_bread, "2026-10-07"), 25000) << "100 + 10 - 60 - 5 - 20";
    EXPECT_EQ(qty(m_bread, "2026-10-08"), 24000) << "до перепроведения инвентаризация вычитает старую разницу 1 шт";

    auto const state = Reposter{m_db}.state();
    ASSERT_TRUE(state.from) << "после правки задним числом нужна отметка";
    EXPECT_EQ(state.from->moment, "2026-10-02 08:00:00");
    EXPECT_EQ(state.cause_id, m_production);
    EXPECT_EQ(Reposter{m_db}.count(state.from->moment), 10)
        << "2 поступления, 4 реализации, 2 списания, инвентаризация, возврат; установки цен не перепроводятся";

    auto const last = repost_all(state.from->moment);
    EXPECT_FALSE(last.error) << *last.error;
    EXPECT_FALSE(last.doc) << "дошли до конца";
    EXPECT_FALSE(last.needed) << "отметка снята";
    EXPECT_FALSE(needed());
    EXPECT_EQ(qty(m_bread, "2026-10-08"), 14000) << "после инвентаризации снова насчитанное";
    expect_state(LAST_DAY, expected().back());
}

/**
 * @brief Перепроведение останавливается на документе с ошибкой и продолжается после исправления.
 *
 * Поступление 02.10 увеличено на 10 батонов; пока инвентаризация не перепроведена, после неё 24 шт,
 * и проходит реализация 20 шт на 08.10 21:00. Перепроведение инвентаризации даёт 14 шт - реализации
 * не хватает: остановка на инвентаризации, отметка остаётся. Реализацию отменяют, перепроведение
 * продолжается с инвентаризации и доходит до конца.
 */
TEST_F(CycleTest, RepostStopsOnError) {
    run_cycle();
    auto bigger = m_docs.get(m_production);
    bigger.lines[0].qty = 110000;
    m_docs.update(bigger);
    auto const late_sale = post(make(DocType::SALE, "2026-10-08", "21:00", {{m_bread, 20000, 3000}}));

    auto const from = Reposter{m_db}.state().from->moment;
    auto const stop = repost_all(from);
    ASSERT_TRUE(stop.doc);
    EXPECT_EQ(stop.doc->id, m_inventory) << "остановились на инвентаризации";
    ASSERT_TRUE(stop.error);
    EXPECT_THAT(*stop.error, testing::HasSubstr("«Батон»: остаток на 08.10.2026 21:00 уйдёт в минус"));
    EXPECT_THAT(doc_title(*stop.doc), testing::StrEq("Инвентаризация № 1 от 08.10.2026 20:00"));
    EXPECT_TRUE(needed()) << "не дошли до конца - отметка остаётся";
    EXPECT_EQ(qty(m_bread, "2026-10-08"), 4000) << "инвентаризация осталась прежней: 24 - 20";

    auto const done = m_reposted;
    ASSERT_FALSE(done.empty());
    auto const run = Reposter{m_db}.run();
    ASSERT_TRUE(run) << "прогон, остановленный ошибкой, хранится в базе";
    EXPECT_EQ(run->done, static_cast<std::int64_t>(done.size()));
    EXPECT_EQ(run->error_id, m_inventory);
    EXPECT_THAT(run->error, testing::HasSubstr("уйдёт в минус"));
    ASSERT_TRUE(run->after);
    EXPECT_EQ(run->after->id, done.back()) << "место - последний перепроведённый документ";

    m_docs.unpost(late_sale);
    auto const rest = repost_all(from);
    EXPECT_FALSE(rest.error) << *rest.error;
    EXPECT_EQ(m_reposted.front(), m_inventory) << "продолжили с остановившего документа";
    EXPECT_FALSE(needed());
    EXPECT_FALSE(Reposter{m_db}.run()) << "дошли до конца - прогон удалён";
    expect_state(LAST_DAY, expected().back());
}

/**
 * @brief Себестоимость батона 04.10 исправлена с 12,00 на 13,00: строки документов после неё
 * получают новую себестоимость только при перепроведении, после него отчёт сходится с ценой.
 */
TEST_F(CycleTest, RepostAfterCostFix) {
    run_cycle();
    auto prices = m_docs.list({.types = {DocType::PRICES}, .date_from = "2026-10-04", .date_to = "2026-10-04"});
    auto fix = *std::find_if(prices.begin(), prices.end(), [](auto const &d) { return d.price_type_id == COST_PRICE_TYPE; });
    fix.lines[0].price_kop = 1300;
    m_docs.update(fix);
    EXPECT_EQ(line(m_sale2, m_bread).first, 1200) << "до перепроведения строка со старой себестоимостью";
    ASSERT_TRUE(needed());
    EXPECT_EQ(Reposter{m_db}.state().cause_id, fix.id);

    EXPECT_FALSE(repost_all("2026-10-04").error);
    EXPECT_EQ(line(m_sale2, m_bread).first, 1300);
    EXPECT_EQ(line(m_return, m_bread).first, 1300);
    EXPECT_EQ(line(m_production, m_bread).first, 1000) << "до 04.10 себестоимость прежняя";
    EXPECT_FALSE(needed());
    auto const r = goods_report(m_db, "2026-10-01", LAST_DAY);
    EXPECT_EQ(r.closing_sum, 60 * 1300 + 30000) << "60 батонов × 13,00 + 2 кг печенья × 150,00";
}

/**
 * @brief Цена реализации изменена после возврата: возврат берёт новую цену при перепроведении.
 */
TEST_F(CycleTest, RepostReturnPrice) {
    run_cycle();
    auto sale = m_docs.get(m_sale);
    sale.lines[0].price_kop = 2600;
    m_docs.update(sale);
    ASSERT_TRUE(needed()) << "у реализации есть проведённый возврат";
    EXPECT_EQ(line(m_return, m_bread).second, 2500);
    EXPECT_FALSE(repost_all("2026-10-02").error);
    EXPECT_EQ(line(m_return, m_bread).second, 2600);
}

/**
 * @brief Отметку не ставят: розничные цены задним числом (не хранятся в документах), правка черновика,
 * складской документ без проведённой инвентаризации после него.
 */
TEST_F(CycleTest, NoMarkWithoutDependents) {
    run_cycle();
    set_prices(m_retail, "2026-10-03", "12:00", {{m_bread, 0, 2700}});
    EXPECT_FALSE(needed()) << "розница задним числом";
    auto draft = make(DocType::WRITEOFF, "2026-10-03", "12:00", {{m_bread, 1000}}, "брак");
    draft.id = m_docs.create(draft);
    draft.lines[0].qty = 2000;
    m_docs.update(draft);
    EXPECT_FALSE(needed()) << "черновик задним числом";
    post(make(DocType::WRITEOFF, "2026-10-09", "12:00", {{m_bread, 1000}}, "брак"));
    EXPECT_FALSE(needed()) << "списание 09.10: инвентаризация 08.10 раньше";
    m_docs.post(draft.id);
    EXPECT_TRUE(needed()) << "списание 03.10 раньше инвентаризации 08.10";
}

/**
 * @brief Отметка, поставленная во время перепроведения, не снимается им; перепроведение, начатое
 * позже отметки, её тоже не снимает.
 */
TEST_F(CycleTest, RepostKeepsNewerMark) {
    run_cycle();
    auto bigger = m_docs.get(m_production);
    bigger.lines[0].qty = 110000;
    m_docs.update(bigger);

    auto const first = Reposter{m_db}.step("2026-10-02");
    ASSERT_TRUE(first.doc);
    post(make(DocType::WRITEOFF, "2026-10-03", "12:00", {{m_bread, 1000}}, "брак"));
    auto const rest = repost_all("2026-10-02");
    ASSERT_FALSE(rest.error) << *rest.error;
    EXPECT_TRUE(rest.needed) << "во время перепроведения появилась новая правка задним числом";

    EXPECT_FALSE(repost_all("2026-10-05").error);
    EXPECT_TRUE(needed()) << "начато 05.10, а отметка с 02.10";
    EXPECT_FALSE(repost_all("2026-10-02").error);
    EXPECT_FALSE(needed());
}

/**
 * @brief Прогон продолжается после «перезагрузки» с того же места: уже перепроведённые документы не
 * перепроводятся заново; прогон с другого момента или restart начинает сначала.
 */
TEST_F(CycleTest, RepostResumesFromDatabase) {
    run_cycle();
    auto bigger = m_docs.get(m_production);
    bigger.lines[0].qty = 110000;
    m_docs.update(bigger);
    auto const total = Reposter{m_db}.count("2026-10-02 08:00:00");

    std::vector<std::int64_t> first;
    for (int i = 0; i < 3; ++i) {
        auto const step = Reposter{m_db}.step("2026-10-02 08:00:00");
        ASSERT_TRUE(step.doc);
        EXPECT_EQ(step.done, i + 1) << "счётчик прогона";
        first.push_back(step.doc->id);
    }
    auto run = Reposter{m_db}.run();
    ASSERT_TRUE(run) << "прогон хранится в базе";
    EXPECT_EQ(run->from, "2026-10-02 08:00:00");
    EXPECT_EQ(run->done, 3);
    ASSERT_TRUE(run->after);
    EXPECT_EQ(run->after->id, first.back());
    EXPECT_FALSE(run->updated.empty()) << "время последнего шага";

    auto const resumed = repost_all("2026-10-02 08:00:00");
    EXPECT_FALSE(resumed.error);
    EXPECT_EQ(static_cast<std::int64_t>(m_reposted.size()), total - 3) << "продолжение не повторяет первые 3 документа";
    for (auto const id : first) {
        EXPECT_THAT(m_reposted, testing::Not(testing::Contains(id))) << "документ " << id << " перепроведён повторно";
    }
    EXPECT_EQ(resumed.done, total);
    EXPECT_FALSE(needed());
    EXPECT_FALSE(Reposter{m_db}.run());

    // прогон с 05.10, затем с другого момента - начинается сначала
    ASSERT_TRUE(Reposter{m_db}.step("2026-10-05").doc);
    ASSERT_TRUE(Reposter{m_db}.step("2026-10-05").doc);
    auto const other = Reposter{m_db}.step("2026-10-07");
    EXPECT_EQ(other.done, 1) << "другой момент - новый прогон";
    EXPECT_EQ(Reposter{m_db}.run()->from, "2026-10-07");
    auto const again = Reposter{m_db}.step("2026-10-07", true);
    EXPECT_EQ(again.done, 1) << "restart - сначала";
    EXPECT_EQ(again.doc->id, other.doc->id) << "restart начинает с первого документа";
}
