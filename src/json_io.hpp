#pragma once
#include <cstdint>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>

#include "catalog.hpp"
#include "docs.hpp"
#include "groups.hpp"
#include "partners.hpp"
#include "prices.hpp"
#include "report.hpp"
#include "requirements.hpp"
#include "stock.hpp"

/**
 * @brief Разбирает тело запроса как JSON-объект.
 * @throws UserError если это не JSON или не объект
 */
[[nodiscard]] nlohmann::json parse_object(std::string const &body);

/// @throws UserError если поля нет или оно не строка
[[nodiscard]] std::string get_string(nlohmann::json const &j, std::string_view key);
/// @throws UserError если поля нет или оно не целое число
[[nodiscard]] std::int64_t get_int(nlohmann::json const &j, std::string_view key);
/// @throws UserError если поле есть, но не логическое
[[nodiscard]] std::optional<bool> get_bool(nlohmann::json const &j, std::string_view key);

[[nodiscard]] nlohmann::json to_json(Item const &item);
[[nodiscard]] nlohmann::json to_json(Recipe const &recipe);

/**
 * @brief Поля позиции из запроса поверх base: отсутствующие поля остаются как в base.
 */
[[nodiscard]] Item item_from_json(nlohmann::json const &j, Item base);

/// Рецептура из {"output_qty": N, "lines": [{"component_id": id, "qty": N}, ...]}.
[[nodiscard]] Recipe recipe_from_json(nlohmann::json const &j);

[[nodiscard]] nlohmann::json to_json(Document const &doc);
[[nodiscard]] nlohmann::json to_json(Requirements const &req);

/**
 * @brief Документ из {"type", "date", "comment", "price_type_id", "lines": [{"item_id", "qty", "price_kop"}]}.
 *
 * type необязателен (для изменения существующего документа), comment по умолчанию пустой.
 */
[[nodiscard]] Document document_from_json(nlohmann::json const &j);

[[nodiscard]] nlohmann::json to_json(Balance const &balance);

[[nodiscard]] nlohmann::json to_json(Group const &group);

/// Группа из {"name", "parent_id": id | null}; parent_id необязателен.
[[nodiscard]] Group group_from_json(nlohmann::json const &j);

/**
 * @brief Необязательный id: поля нет - base, null - пусто, число - значение.
 * @throws UserError если значение не целое и не null
 */
[[nodiscard]] std::optional<std::int64_t> get_optional_id(nlohmann::json const &j, std::string_view key, std::optional<std::int64_t> base);

[[nodiscard]] nlohmann::json to_json(PriceType const &type);
[[nodiscard]] nlohmann::json to_json(PriceRow const &row);

[[nodiscard]] nlohmann::json to_json(Partner const &partner);
/// Поля клиента из запроса поверх base; отсутствующие остаются как в base.
[[nodiscard]] Partner partner_from_json(nlohmann::json const &j, Partner base);

[[nodiscard]] nlohmann::json to_json(Returnable const &r);

[[nodiscard]] nlohmann::json to_json(GoodsReport const &report);
