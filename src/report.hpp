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
    std::int64_t sum{};
    DocType type = DocType::PRODUCTION;
    bool incoming = true;
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
};

/**
 * @brief Строит товарный отчёт за период [from, to] (даты включительно) по проведённым документам.
 *
 * Суммы - по себестоимости из строк документов: стоимость движения = количество × себестоимость
 * строки, округление до копейки по документу. Остаток на конец = начало + приход - расход; суммы
 * по позициям (приложение) округляются отдельно и могут отличаться от итога на копейки.
 * Позиции без движений и остатков не попадают.
 * @param group позиции группы и её подгрупп
 * @throws UserError неверные даты или from > to
 */
[[nodiscard]] GoodsReport goods_report(Database &db, std::string const &from, std::string const &to,
                                       std::optional<std::int64_t> group = std::nullopt);
