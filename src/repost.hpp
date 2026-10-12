#pragma once
#include <cstdint>
#include <optional>
#include <string>

#include "db.hpp"
#include "docs.hpp"

/**
 * @brief Место документа в порядке проведения: момент, при равном моменте - id.
 */
struct DocKey {
    std::string moment; ///< "ГГГГ-ММ-ДД ЧЧ:ММ:СС"
    std::int64_t id{};
};

/**
 * @brief Нужно ли перепровести документы после изменения задним числом.
 */
struct RepostState {
    std::optional<DocKey> from; ///< перепровести документы начиная с этого места; nullopt - не нужно
    std::optional<std::int64_t> cause_id; ///< документ, изменённый задним числом (самый ранний)
    std::int64_t epoch{}; ///< растёт при каждой отметке: перепроведение, начатое раньше, отметку не снимает
};

/**
 * @brief Результат одного шага перепроведения.
 */
struct RepostStep {
    std::optional<Document> doc; ///< перепроведённый или (при ошибке) остановивший документ; nullopt - документов не осталось
    std::optional<std::string> error; ///< причина остановки
    std::int64_t remaining{};         ///< документов после doc
    bool needed = false;              ///< отметка о перепроведении осталась (после последнего шага)
};

/**
 * @brief Перепроведение документов по порядку, начиная с момента.
 *
 * Перепроводятся проведённые складские документы: строки получают себестоимость на свой момент,
 * возврат - цену из реализации, инвентаризация - разницу с учётом на свой момент. Каждый документ -
 * в своей транзакции; на ошибке перепроведение останавливается на этом документе.
 *
 * Отметка «нужно перепровести» ставится документами (Documents) при изменении задним числом и
 * снимается, когда перепроведение, начатое не позже отметки и без новых отметок за время работы,
 * дошло до конца.
 *
 * Методы не захватывают блокировку базы: это делает вызывающий.
 */
class Reposter {
   public:
    explicit Reposter(Database &db);

    [[nodiscard]] RepostState state() const;

    /**
     * @brief Отмечает, что документы начиная с key нужно перепровести; остаётся более ранняя отметка.
     */
    void mark(DocKey const &key, std::int64_t cause_id);

    /// Сколько документов перепроводится начиная с момента from.
    [[nodiscard]] std::int64_t count(std::string const &from) const;

    /**
     * @brief Перепроводит следующий документ.
     * @param from момент начала перепроведения
     * @param after последний перепроведённый документ; nullopt - начать с from
     * @param epoch RepostState::epoch на начало перепроведения
     */
    RepostStep step(std::string const &from, std::optional<DocKey> const &after, std::int64_t epoch);

   private:
    Database &m_db;
};
