#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "catalog.hpp"
#include "db.hpp"

/**
 * @brief Вид документа.
 */
enum class DocType : std::uint8_t {
    ORDER,      ///< заявка на производство, движений не делает
    PRODUCTION, ///< поступление из производства
    SALE,       ///< реализация
    RETURN,     ///< возврат от покупателя
    WRITEOFF,   ///< списание
    INVENTORY,  ///< инвентаризация
};

[[nodiscard]] std::string_view to_string(DocType type);
/// @throws UserError для неизвестного значения
[[nodiscard]] DocType doc_type_from_string(std::string_view text);

/**
 * @brief Строка документа.
 */
struct DocLine {
    std::string name; ///< только для чтения
    std::int64_t item_id{};
    std::int64_t qty{};    ///< тысячные доли единицы позиции
    Unit unit = Unit::PCS; ///< только для чтения
};

/**
 * @brief Документ с заголовком и строками.
 */
struct Document {
    std::vector<DocLine> lines;
    std::string date; ///< YYYY-MM-DD
    std::string comment;
    std::string status = "draft"; ///< только для чтения
    std::int64_t id{};
    std::int64_t number{}; ///< только для чтения: присваивается при создании, сквозной в пределах вида
    DocType type = DocType::ORDER;
};

/**
 * @brief Фильтр списка документов; даты включительно.
 */
struct DocFilter {
    std::optional<DocType> type;
    std::optional<std::string> date_from;
    std::optional<std::string> date_to;
};

/**
 * @brief Проверяет дату формата YYYY-MM-DD, включая существование дня.
 * @throws UserError
 */
void check_date(std::string_view date);

/**
 * @brief Документы. Сейчас поддерживаются заявки; остальные виды добавляются вместе с проведением.
 *
 * Методы не захватывают блокировку базы: это делает вызывающий.
 */
class Documents {
   public:
    explicit Documents(Database &db);

    /// Заголовки и строки документов по фильтру, по дате и номеру.
    [[nodiscard]] std::vector<Document> list(DocFilter const &filter) const;

    /// @throws NotFoundError
    [[nodiscard]] Document get(std::int64_t id) const;

    /**
     * @brief Создаёт документ со следующим номером своего вида.
     * @throws UserError неверная дата, пустые строки, позиция не того вида или в архиве
     * @return id
     */
    std::int64_t create(Document const &doc);

    /**
     * @brief Заменяет дату, комментарий и строки.
     * @throws NotFoundError, UserError проведённый документ или неверные данные
     */
    void update(Document const &doc);

    /// @throws NotFoundError, UserError для проведённого документа
    void remove(std::int64_t id);

   private:
    /// Проверки вида документа, даты и строк; old_items - позиции, уже бывшие в документе.
    void validate(Document const &doc, std::vector<std::int64_t> const &old_items) const;

    void write_lines(std::int64_t doc_id, std::vector<DocLine> const &lines);

    Database &m_db;
};
