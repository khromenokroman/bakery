#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "db.hpp"

/**
 * @brief Клиент: магазин или частное лицо.
 */
struct Partner {
    std::string name;
    std::string phone;
    std::string note;
    std::int64_t id{};
    bool archived = false;
};

/**
 * @brief Справочник клиентов. Название уникально. Методы не захватывают блокировку базы.
 */
class Partners {
   public:
    explicit Partners(Database &db);

    /// По названию; архивные только с with_archived.
    [[nodiscard]] std::vector<Partner> list(bool with_archived) const;
    /// @throws NotFoundError
    [[nodiscard]] Partner get(std::int64_t id) const;
    /// @throws UserError пустое или занятое название
    std::int64_t create(Partner const &partner);
    /// @throws UserError, NotFoundError
    void update(Partner const &partner);
    /// @throws UserError если клиент есть в документах (тогда его отправляют в архив)
    void remove(std::int64_t id);

   private:
    [[nodiscard]] Partner clean(Partner const &partner) const;

    Database &m_db;
};
