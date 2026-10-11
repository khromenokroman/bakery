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
    PRICES,     ///< установка цен одного типа, движений не делает
};

[[nodiscard]] std::string_view to_string(DocType type);
/// @throws UserError для неизвестного значения
[[nodiscard]] DocType doc_type_from_string(std::string_view text);

/**
 * @brief Строка документа.
 */
struct DocLine {
    std::string name;                      ///< только для чтения
    std::optional<std::int64_t> price_kop; ///< цена за единицу: установка цен, реализация
    std::optional<std::int64_t> cost_kop; ///< себестоимость за единицу на дату документа; заполняет сервер
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
    std::string status = "draft";              ///< только для чтения
    std::optional<std::int64_t> price_type_id; ///< тип цен: установка цен, реализация
    std::optional<std::int64_t> partner_id;    ///< клиент: заявка, реализация
    std::string partner_name;                  ///< только для чтения
    std::int64_t id{};
    std::int64_t number{}; ///< только для чтения: присваивается при создании, сквозной в пределах вида
    DocType type = DocType::ORDER;
};

/**
 * @brief Фильтр списка документов; даты включительно.
 */
struct DocFilter {
    std::vector<DocType> types; ///< пустой - все виды
    std::optional<std::string> date_from;
    std::optional<std::string> date_to;
};

/**
 * @brief Проверяет дату формата YYYY-MM-DD, включая существование дня.
 * @throws UserError
 */
void check_date(std::string_view date);

/**
 * @brief Документы: заявки (без движений), поступление из производства, реализация, установка цен.
 *
 * В строки складских документов при сохранении записывается себестоимость на дату документа
 * (тип цен «Себестоимость»); установка цен действует после проведения.
 *
 * Проведение пишет движения склада, отмена проведения их удаляет; остаток не может уйти в минус
 * ни на один день. Изменение проведённого документа перепроводит его; удалить проведённый нельзя.
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
     *
     * Проведённый документ перепроводится в той же транзакции: старые движения заменяются новыми,
     * минус проверяется только по итогу (по позициям и старой, и новой версии). При отказе документ
     * остаётся прежним.
     * @throws NotFoundError, UserError неверные данные или остаток уйдёт в минус
     */
    void update(Document const &doc);

    /// @throws NotFoundError, UserError для проведённого документа
    void remove(std::int64_t id);

    /**
     * @brief Проводит документ: движения склада по строкам на дату документа.
     * @throws NotFoundError, UserError уже проведён, вид без движений, остаток уйдёт в минус
     */
    void post(std::int64_t id);

    /**
     * @brief Отменяет проведение: удаляет движения документа.
     * @throws NotFoundError, UserError не проведён, остаток уйдёт в минус
     */
    void unpost(std::int64_t id);

   private:
    /// Проверки вида документа, даты и строк; old_items - позиции, уже бывшие в документе.
    void validate(Document const &doc, std::vector<std::int64_t> const &old_items) const;

    /// Строки документа doc.id; себестоимость для складских видов берётся на doc.date.
    void write_lines(Document const &doc);

    /**
     * @brief Проверки перед проведением: в реализации у каждой строки должна быть цена.
     * @throws UserError со списком строк без цены
     */
    void check_postable(Document const &doc) const;

    /// Движения склада по строкам документа на его дату, со знаком его вида.
    void write_moves(Document const &doc);

    Database &m_db;
};
