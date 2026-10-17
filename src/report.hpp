#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "catalog.hpp"
#include "db.hpp"
#include "docs.hpp"

/**
 * @brief Движение позиции за период: количество в тысячных, суммы по себестоимости в копейках.
 */
struct ReportItem {
    std::string name;
    std::int64_t item_id{};
    std::int64_t opening{};
    std::int64_t in{};
    std::int64_t out{};
    std::int64_t closing{};
    std::int64_t opening_sum{};
    std::int64_t closing_sum{};
    Unit unit = Unit::PCS;
};

/**
 * @brief Документ периода с суммой по себестоимости (приход или расход).
 */
struct ReportDoc {
    std::string date; ///< ГГГГ-ММ-ДД
    std::string time; ///< ЧЧ:ММ:СС
    std::string partner_name;
    std::int64_t id{};
    std::int64_t number{};
    std::int64_t sum{};       ///< по себестоимости
    std::int64_t price_sum{}; ///< по выбранному типу цен
    DocType type = DocType::PRODUCTION;
    bool incoming = true;
    bool revaluation = false; ///< переоценка по цене продажи: строка к реализации с суммой только по типу цен
};

/**
 * @brief Товарный отчёт за период по образцу ТОРГ-29.
 */
struct GoodsReport {
    std::vector<ReportDoc> incoming;
    std::vector<ReportDoc> outgoing;
    std::vector<ReportItem> items;
    std::int64_t opening_sum{};
    std::int64_t in_sum{};
    std::int64_t out_sum{};
    std::int64_t closing_sum{};
    std::int64_t lines_without_cost{}; ///< строк движений до конца периода без себестоимости (оценены нулём)

    std::optional<std::int64_t> price_type_id; ///< вторая оценка: по типу цен, если выбран
    std::int64_t opening_price_sum{};
    std::int64_t in_price_sum{};
    std::int64_t out_price_sum{};
    std::int64_t closing_price_sum{};
    std::int64_t moves_without_price{}; ///< движений до конца периода без цены этого типа (оценены нулём)
};

/**
 * @brief Строка отчёта о продажах: товар и его за период суммы в копейках.
 *
 * qty - нетто (реализация минус возврат) в тысячных единицы; margin = revenue - cost.
 * margin_pct - наценка в процентах к себестоимости, 0, если себестоимости нет.
 */
struct SalesLine {
    std::int64_t item_id{};
    std::string name;
    Unit unit = Unit::PCS;
    std::string group_name;
    std::int64_t group_id{};
    std::int64_t qty{};        ///< нетто, тысячные доли единицы
    std::int64_t cost{};       ///< себестоимость, копейки
    std::int64_t revenue{};    ///< цена продажи, копейки
    std::int64_t margin{};     ///< наценка = revenue - cost, копейки
    std::int64_t margin_pct{}; ///< наценка в % к себестоимости, 0 если себестоимости нет
};

/**
 * @brief Группа товаров в отчёте о продажах с промежуточными итогами.
 */
struct SalesGroup {
    std::int64_t group_id{}; ///< 0 - товары без группы
    std::string name;
    std::vector<SalesLine> items;
    std::int64_t qty{};
    std::int64_t cost{};
    std::int64_t revenue{};
    std::int64_t margin{};
    std::int64_t margin_pct{};
};

/**
 * @brief Отчёт о продажах за период: себестоимость, выручка и наценка по группам.
 */
struct SalesReport {
    std::vector<SalesGroup> groups;
    std::int64_t total_qty{};
    std::int64_t total_cost{};
    std::int64_t total_revenue{};
    std::int64_t total_margin{}; ///< total_revenue - total_cost
    std::int64_t total_margin_pct{};
    std::int64_t lines_without_cost{}; ///< строк реализации/возврата без себестоимости (оценены нулём)
    std::int64_t lines_without_price{}; ///< строк реализации/возврата без цены продажи (оценены нулём)
};

/**
 * @brief Продажи за период [from, to] (даты включительно): реализация минус возврат,
 * разбивка по группам товаров, итог по себестоимости, цене продажи и наценке.
 * @param group позиции группы и её подгрупп; пусто - все группы
 * @throws UserError неверные даты или from > to
 */
[[nodiscard]] SalesReport sales_report(Database &db, std::string const &from, std::string const &to,
                                       std::optional<std::int64_t> group = std::nullopt);

/**
 * @brief Строит товарный отчёт за период [from, to] (даты включительно) по проведённым документам.
 *
 * Суммы - по себестоимости из строк документов: стоимость движения = количество × себестоимость
 * строки, округление до копейки по документу. Остаток на конец = начало + приход - расход; суммы
 * по позициям (приложение) округляются отдельно и могут отличаться от итога на копейки.
 * Позиции без движений и остатков не попадают.
 *
 * Если задан price_type, суммы считаются ещё и по ценам этого типа: каждое движение по цене,
 * действовавшей на момент его документа, остаток на конец - по той же арифметике. Реализация по типу цен
 * списывается по фактической цене продажи, а разница с ценой типа (продано × (цена продажи - цена типа))
 * идёт строками переоценки к этой реализации: продано дороже - приход, дешевле - расход (в одной реализации
 * могут быть обе строки).
 * Так расход по реализации равен её сумме, а остаток по-прежнему равен количеству × цене типа.
 *
 * Установка цен (себестоимости и выбранного типа) переоценивает остаток: остаток до её момента ×
 * (новая цена - прежняя). В периоде переоценка - строка документа установки цен (рост - приход,
 * снижение - расход), до периода - входит в остаток на начало. Поэтому остаток на конец равен
 * количеству × цене на конец периода.
 * @param group позиции группы и её подгрупп
 * @param price_type тип цен для второй оценки
 * @throws UserError неверные даты или from > to
 */
[[nodiscard]] GoodsReport goods_report(Database &db, std::string const &from, std::string const &to, std::optional<std::int64_t> group = std::nullopt,
                                       std::optional<std::int64_t> price_type = std::nullopt);
