#pragma once
#include <optional>
#include <string>

#include "db.hpp"

/**
 * @brief Закрытие периода: дата, по которую (включительно) данные менять нельзя.
 *
 * В закрытом периоде документы любого вида нельзя создавать, изменять, удалять, проводить, отменять и
 * перепроводить; изменение документа не может ни начинаться, ни заканчиваться в закрытом периоде.
 * Дата хранится в таблице settings под ключом closed_till. Методы не захватывают блокировку базы.
 */
class Period {
   public:
    explicit Period(Database &db);

    /// Дата закрытия "ГГГГ-ММ-ДД"; nullopt - ничего не закрыто.
    [[nodiscard]] std::optional<std::string> closed_till() const;

    /**
     * @brief Меняет дату закрытия; nullopt открывает все периоды.
     *
     * Закрыть период нельзя, пока документы в нём ждут перепроведения (отметка Reposter не позже даты):
     * после закрытия перепровести их было бы нельзя. Открыть период (сдвинуть дату назад) можно всегда.
     * @throws UserError неверная дата или незавершённое перепроведение в закрываемом периоде
     */
    void close(std::optional<std::string> const &till);

    /// Момент "ГГГГ-ММ-ДД ЧЧ:ММ:СС" или дата попадает в закрытый период.
    [[nodiscard]] bool is_closed(std::string const &moment) const;

    /**
     * @brief Проверяет, что момент не в закрытом периоде.
     * @param moment "ГГГГ-ММ-ДД ЧЧ:ММ:СС" или "ГГГГ-ММ-ДД"
     * @param what что нельзя сделать, для сообщения: "изменить документ от 15.09.2026"
     * @throws UserError
     */
    void check_open(std::string const &moment, std::string const &what) const;

   private:
    Database &m_db;
};
