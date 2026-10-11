#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "catalog.hpp"
#include "db.hpp"

/**
 * @brief Остаток позиции на складе.
 */
struct Balance {
    std::string name;
    std::int64_t item_id{};
    std::int64_t qty{}; ///< тысячные доли единицы
    Unit unit = Unit::PCS;
    bool archived = false;
};

/**
 * @brief Количество с единицей для сообщений: 1500, kg -> "1,5 кг".
 */
[[nodiscard]] std::string format_qty(std::int64_t milli, Unit unit);

/**
 * @brief Склад готовой продукции: остатки как сумма движений проведённых документов.
 *
 * Методы не захватывают блокировку базы: это делает вызывающий.
 */
class Stock {
   public:
    explicit Stock(Database &db);

    /**
     * @brief Ненулевые остатки на конец дня date (все движения, если nullopt), по названию.
     */
    [[nodiscard]] std::vector<Balance> balances(std::optional<std::string> const &date) const;

    /**
     * @brief Проверяет, что остаток каждой позиции из items не меньше нуля на конец любого дня.
     *
     * Вызывается в транзакции после записи движений; исключение откатывает транзакцию.
     * @throws UserError с названием, датой и недостающим количеством
     */
    void check_non_negative(std::vector<std::int64_t> const &items) const;

   private:
    Database &m_db;
};
