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
    std::string date;              ///< YYYY-MM-DD
    std::string time = "00:00:00"; ///< ЧЧ:ММ:СС; вместе с датой задаёт момент документа
    std::string comment;
    std::string status = "draft";              ///< только для чтения
    std::optional<std::int64_t> price_type_id; ///< тип цен: установка цен, реализация
    std::optional<std::int64_t> partner_id;    ///< клиент: заявка, реализация
    std::string partner_name;                  ///< только для чтения
    std::optional<std::int64_t> base_id;       ///< основание: реализация для возврата
    std::optional<std::int64_t> group_id; ///< область инвентаризации: группа с подгруппами; пусто - вся продукция
    std::int64_t id{};
    std::int64_t number{}; ///< только для чтения: присваивается при создании, сквозной в пределах вида
    DocType type = DocType::ORDER;
};

/**
 * @brief Сколько по строке реализации можно вернуть.
 */
struct Returnable {
    std::string name;
    std::optional<std::int64_t> price_kop;
    std::optional<std::int64_t> cost_kop;
    std::int64_t item_id{};
    std::int64_t sold{};     ///< продано, тысячные доли
    std::int64_t returned{}; ///< возвращено проведёнными возвратами (кроме исключённого)
    Unit unit = Unit::PCS;
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
 * @brief Время "ЧЧ:ММ" или "ЧЧ:ММ:СС" в виде "ЧЧ:ММ:СС".
 * @throws UserError
 */
[[nodiscard]] std::string normalize_time(std::string_view time);

/// Момент документа для базы и сравнения: "ГГГГ-ММ-ДД ЧЧ:ММ:СС".
[[nodiscard]] std::string moment(Document const &doc);

/// Момент для сообщений: "12.10.2026 09:30", без времени, если оно 00:00:00.
[[nodiscard]] std::string ru_moment(std::string const &moment);

/**
 * @brief Документы: заявки (без движений), поступление из производства, реализация, возврат по реализации,
 * списание, инвентаризация, установка цен.
 *
 * Инвентаризация: в строках фактическое количество; при проведении для каждой позиции области
 * движение = факт - учётный остаток на момент документа (без его собственных движений), позиции
 * области, которых нет в документе, обнуляются.
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
     * @brief Строки реализации с проданным и уже возвращённым количеством.
     * @param exclude_return возврат, который не учитывать (редактируемый)
     * @throws NotFoundError; UserError если документ не реализация
     */
    [[nodiscard]] std::vector<Returnable> returnable(std::int64_t sale_id, std::optional<std::int64_t> exclude_return = std::nullopt) const;

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

    /**
     * @brief Возврат: основание - проведённая реализация не позже даты возврата, позиции из неё,
     * количество не больше непроданного остатка по ней.
     * @throws UserError
     */
    void check_return(Document const &doc) const;

    /**
     * @brief Реализацию нельзя отменить или уменьшить ниже возвращённого проведёнными возвратами.
     * @param sale новая версия реализации или nullopt при отмене проведения
     * @throws UserError
     */
    void check_sale_returns(std::int64_t sale_id, std::optional<Document> const &sale) const;

    /// Движения склада по строкам документа на его дату, со знаком его вида.
    void write_moves(Document const &doc);

    /// Позиции, по которым у документа есть движения склада.
    [[nodiscard]] std::vector<std::int64_t> moved_items(std::int64_t doc_id) const;

    Database &m_db;
};
