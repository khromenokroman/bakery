#pragma once
#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "db.hpp"

/**
 * @brief Группа номенклатуры; parent_id пустой у групп верхнего уровня.
 */
struct Group {
    std::string name;
    std::optional<std::int64_t> parent_id;
    std::int64_t id{};
};

/**
 * @brief Дерево групп номенклатуры.
 *
 * Название уникально среди групп с тем же родителем. Методы не захватывают блокировку базы.
 */
class Groups {
   public:
    explicit Groups(Database &db);

    /// Все группы, по названию; дерево строит интерфейс по parent_id.
    [[nodiscard]] std::vector<Group> list() const;

    /// @throws NotFoundError
    [[nodiscard]] Group get(std::int64_t id) const;

    /**
     * @throws UserError пустое или занятое у соседей название; NotFoundError нет родителя
     * @return id новой группы
     */
    std::int64_t create(Group const &group);

    /**
     * @brief Переименовывает и/или переносит группу.
     * @throws UserError группа становится вложенной в саму себя, занятое название; NotFoundError
     */
    void update(Group const &group);

    /// @throws UserError если в группе есть подгруппы или позиции
    void remove(std::int64_t id);

    /// Группа и все её подгруппы на любой глубине.
    [[nodiscard]] std::set<std::int64_t> with_descendants(std::int64_t id) const;

    /// Путь от корня: "Кондитерка / Торты".
    [[nodiscard]] std::string path(std::int64_t id) const;

   private:
    void check_name(Group const &group) const;

    Database &m_db;
};
