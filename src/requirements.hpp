#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "catalog.hpp"
#include "db.hpp"

/**
 * @brief Потребность в одной позиции.
 */
struct Need {
    std::string name;
    std::int64_t item_id{};
    std::int64_t qty{}; ///< тысячные доли единицы позиции
    Kind kind = Kind::RAW;
    Unit unit = Unit::KG;
    bool has_recipe = false;
};

/**
 * @brief Результат расчёта по набору заявок.
 *
 * Все списки отсортированы по названию.
 */
struct Requirements {
    std::vector<Need> products; ///< продукция из заявок: сколько произвести
    std::vector<Need> intermediates; ///< полуфабрикаты и продукция, входящие в рецептуры других позиций
    std::vector<Need> raw;           ///< сырьё
    std::vector<std::string> warnings;
};

/**
 * @brief Разворачивает заявки по рецептурам до сырья.
 *
 * Спрос на позицию сначала суммируется со всех заявок и рецептур, где она нужна, затем позиция
 * разворачивается один раз (топологический порядок). Доли округляются вверх до тысячной единицы.
 * Итог по штучному сырью округляется вверх до целых штук. Позиция без рецептуры (кроме сырья)
 * остаётся в своём списке и даёт предупреждение.
 *
 * @param doc_ids id заявок; повторы игнорируются
 * @throws NotFoundError нет документа; UserError документ не заявка или пустой список
 */
[[nodiscard]] Requirements compute_requirements(Database &db, std::vector<std::int64_t> const &doc_ids);
