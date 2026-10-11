#pragma once
#include "db.hpp"

/**
 * @brief Версия схемы, которую создаёт эта сборка (число миграций).
 */
[[nodiscard]] int schema_version();

/**
 * @brief Текущая версия схемы в базе (PRAGMA user_version), 0 для пустой базы.
 */
[[nodiscard]] int current_schema_version(Database &db);

/**
 * @brief Применяет недостающие миграции, каждую в своей транзакции.
 *
 * @throws DbError если база создана более новой версией программы
 * @return версия схемы после миграции
 */
int migrate(Database &db);
