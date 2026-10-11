#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "db.hpp"

/**
 * @brief Вид номенклатуры.
 */
enum class Kind : std::uint8_t {
    RAW,     ///< сырьё: только для расчёта потребности, без рецептуры
    SEMI,    ///< полуфабрикат: имеет рецептуру, входит в другие рецептуры
    PRODUCT, ///< продукция: имеет рецептуру, учитывается на складе
};

/**
 * @brief Единица измерения.
 */
enum class Unit : std::uint8_t {
    KG,
    L,
    PCS,
};

[[nodiscard]] std::string_view to_string(Kind kind);
[[nodiscard]] std::string_view to_string(Unit unit);

/// @throws UserError для неизвестного значения
[[nodiscard]] Kind kind_from_string(std::string_view text);
/// @throws UserError для неизвестного значения
[[nodiscard]] Unit unit_from_string(std::string_view text);

/**
 * @brief Позиция номенклатуры.
 */
struct Item {
    std::string name;
    std::optional<std::int64_t> group_id; ///< пустой - без группы
    std::optional<int> vat_rate;          ///< ставка НДС, %; пустая - без НДС
    std::int64_t id{};
    Kind kind = Kind::RAW;
    Unit unit = Unit::KG;
    bool archived = false;
    bool has_recipe = false; ///< только для чтения: заполняется при выборке
};

/**
 * @brief Строка рецептуры: компонент и его количество в тысячных долях единицы компонента.
 */
struct RecipeLine {
    std::string name; ///< только для чтения
    std::int64_t component_id{};
    std::int64_t qty{};
    Kind kind = Kind::RAW; ///< только для чтения
    Unit unit = Unit::KG;  ///< только для чтения
};

/**
 * @brief Рецептура: на выход output_qty (тысячные доли единицы изделия) расходуются lines.
 */
struct Recipe {
    std::vector<RecipeLine> lines;
    std::int64_t output_qty{};
};

/**
 * @brief Фильтр списка номенклатуры.
 */
struct ItemFilter {
    std::optional<Kind> kind;
    std::optional<std::int64_t> group; ///< группа вместе со всеми подгруппами
    bool with_archived = false;
};

/**
 * @brief Справочник номенклатуры и рецептур.
 *
 * Методы не захватывают блокировку базы: это делает вызывающий.
 */
class Catalog {
   public:
    explicit Catalog(Database &db);

    /// Список, отсортированный по виду и названию.
    [[nodiscard]] std::vector<Item> list(ItemFilter const &filter) const;

    /// @throws NotFoundError
    [[nodiscard]] Item get(std::int64_t id) const;

    /**
     * @brief Создаёт позицию; название обрезается по краям.
     * @throws UserError пустое или занятое название
     * @return id новой позиции
     */
    std::int64_t create(Item const &item);

    /**
     * @brief Изменяет название, вид, единицу, группу и признак архива.
     *
     * Сырьё не может иметь рецептуру. Единицу нельзя менять, если позиция входит в рецептуры
     * или имеет свою: количества в них записаны в старой единице.
     * @throws NotFoundError, UserError
     */
    void update(Item const &item);

    /**
     * @brief Удаляет позицию вместе с её рецептурой и ценами.
     * @throws UserError если позиция входит в рецептуры или документы (тогда её архивируют)
     */
    void remove(std::int64_t id);

    /// @return рецептура или nullopt, если её нет
    [[nodiscard]] std::optional<Recipe> recipe(std::int64_t item_id) const;

    /**
     * @brief Заменяет рецептуру позиции целиком.
     *
     * Проверяет: позиция не сырьё, выход и количества больше нуля, компоненты существуют,
     * не повторяются, новые компоненты не в архиве, нет циклов. Компонентом может быть позиция
     * любого вида: полуфабрикат или продукция разворачиваются по своей рецептуре.
     * @throws NotFoundError, UserError
     */
    void set_recipe(std::int64_t item_id, Recipe const &recipe);

    /// Удаляет рецептуру; если её нет, ничего не делает.
    void remove_recipe(std::int64_t item_id);

   private:
    /// @throws NotFoundError если группа указана, но её нет
    void check_group(std::optional<std::int64_t> group) const;

    /// Путь от from до to по рецептурам (from, ..., to) или пустой, если to недостижим.
    [[nodiscard]] std::vector<std::int64_t> find_path(std::int64_t from, std::int64_t to) const;

    /// Названия позиций через separator; отсутствующие как #id.
    [[nodiscard]] std::string names_of(std::vector<std::int64_t> const &ids, std::string_view separator) const;

    Database &m_db;
};
