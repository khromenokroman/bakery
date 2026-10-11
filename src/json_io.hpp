#pragma once
#include <cstdint>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>

#include "catalog.hpp"
#include "docs.hpp"
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
 * @brief Документ из {"type", "date", "comment", "lines": [{"item_id", "qty"}]}.
 *
 * type необязателен (для изменения существующего документа), comment по умолчанию пустой.
 */
[[nodiscard]] Document document_from_json(nlohmann::json const &j);

[[nodiscard]] nlohmann::json to_json(Balance const &balance);
