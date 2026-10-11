#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "catalog.hpp"
#include "db.hpp"

/// Встроенный тип цен «Себестоимость»: подставляется в строки складских документов.
constexpr std::int64_t COST_PRICE_TYPE = 1;

/**
 * @brief Тип цен; встроенный (system) нельзя удалить или переименовать.
 */
struct PriceType {
    std::string name;
    std::int64_t id{};
    bool system = false;
};

/**
 * @brief Действующая цена позиции.
 */
struct PriceRow {
    std::string name;
    std::string date; ///< дата документа «Установка цен», с которой действует цена
    std::int64_t item_id{};
    std::int64_t price_kop{};
    std::int64_t doc_id{};
    Unit unit = Unit::PCS;
};

/**
 * @brief Цены как результат проведённых документов «Установка цен».
 *
 * Цена позиции по типу на дату - из последнего проведённого документа этого типа с датой не позже
 * заданной (при равной дате - из документа с большим id). Методы не захватывают блокировку базы.
 */
class Prices {
   public:
    explicit Prices(Database &db);

    [[nodiscard]] std::vector<PriceType> types() const;
    /// @throws NotFoundError
    [[nodiscard]] PriceType type(std::int64_t id) const;
    /// @throws UserError пустое или занятое название
    std::int64_t create_type(std::string const &name);
    /// @throws UserError встроенный тип, пустое или занятое название; NotFoundError
    void rename_type(std::int64_t id, std::string const &name);
    /// @throws UserError встроенный тип или есть документы с этим типом; NotFoundError
    void remove_type(std::int64_t id);

    /// Цена позиции по типу на момент "ГГГГ-ММ-ДД ЧЧ:ММ:СС" (одна дата - конец дня); nullopt, если цены нет.
    [[nodiscard]] std::optional<std::int64_t> price(std::int64_t type_id, std::int64_t item_id, std::string const &moment) const;

    /// Все позиции с действующей ценой этого типа на момент (одна дата ГГГГ-ММ-ДД - конец дня), по названию.
    [[nodiscard]] std::vector<PriceRow> price_list(std::int64_t type_id, std::string const &date) const;

   private:
    [[nodiscard]] std::string check_name(std::string const &name, std::int64_t self) const;

    Database &m_db;
};
