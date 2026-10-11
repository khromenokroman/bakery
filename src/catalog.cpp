#include "catalog.hpp"

#include <array>
#include <map>
#include <set>
#include <utility>

#include "errors.hpp"
#include "groups.hpp"

namespace {

/// Верхняя граница количества: миллион единиц, чтобы произведения в расчётах не переполняли int64.
constexpr std::int64_t MAX_QTY = 1'000'000'000;
constexpr std::size_t MAX_NAME = 200;

constexpr std::array<std::pair<Kind, std::string_view>, 3> KINDS = {{
    {Kind::RAW, "raw"},
    {Kind::SEMI, "semi"},
    {Kind::PRODUCT, "product"},
}};

constexpr std::array<std::pair<Unit, std::string_view>, 3> UNITS = {{
    {Unit::KG, "kg"},
    {Unit::L, "l"},
    {Unit::PCS, "pcs"},
}};

std::string trim(std::string_view text) {
    auto const first = text.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) {
        return {};
    }
    auto const last = text.find_last_not_of(" \t\r\n");
    return std::string{text.substr(first, last - first + 1)};
}

std::string check_name(std::string_view raw) {
    auto name = trim(raw);
    if (name.empty()) {
        throw UserError("название не может быть пустым");
    }
    if (name.size() > MAX_NAME) {
        throw UserError("название длиннее " + std::to_string(MAX_NAME) + " байт");
    }
    return name;
}

void check_qty(std::int64_t qty, std::string const &what) {
    if (qty <= 0) {
        throw UserError(what + ": количество должно быть больше нуля");
    }
    if (qty > MAX_QTY) {
        throw UserError(what + ": количество больше миллиона единиц");
    }
}

std::int64_t count(Database &db, std::string_view sql, std::int64_t id) {
    auto stmt = db.prepare(sql);
    stmt.bind(1, id);
    stmt.step();
    return stmt.column_int(0);
}

Item read_item(Statement const &stmt) {
    Item item;
    item.id = stmt.column_int(0);
    item.name = stmt.column_text(1);
    item.kind = kind_from_string(stmt.column_text(2));
    item.unit = unit_from_string(stmt.column_text(3));
    item.archived = stmt.column_int(4) != 0;
    item.has_recipe = stmt.column_int(5) != 0;
    if (!stmt.column_is_null(6)) {
        item.group_id = stmt.column_int(6);
    }
    if (!stmt.column_is_null(7)) {
        item.vat_rate = static_cast<int>(stmt.column_int(7));
    }
    return item;
}

constexpr char const SELECT_ITEM[] =
    "SELECT i.id, i.name, i.kind, i.unit, i.archived, r.item_id IS NOT NULL, i.group_id, i.vat_rate FROM items i LEFT JOIN recipes r ON r.item_id = "
    "i.id ";

void check_vat(std::optional<int> rate) {
    if (rate && *rate != 0 && *rate != 5 && *rate != 7 && *rate != 10 && *rate != 20 && *rate != 22) {
        throw UserError("ставка НДС " + std::to_string(*rate) + "%: допустимы 0, 5, 7, 10, 20, 22 или без НДС");
    }
}

void bind_group(Statement &stmt, int index, std::optional<std::int64_t> group) {
    if (group) {
        stmt.bind(index, *group);
    } else {
        stmt.bind_null(index);
    }
}

} // namespace

std::string_view to_string(Kind kind) {
    for (auto const &[k, s] : KINDS) {
        if (k == kind) {
            return s;
        }
    }
    return "?";
}

std::string_view to_string(Unit unit) {
    for (auto const &[u, s] : UNITS) {
        if (u == unit) {
            return s;
        }
    }
    return "?";
}

Kind kind_from_string(std::string_view text) {
    for (auto const &[k, s] : KINDS) {
        if (s == text) {
            return k;
        }
    }
    throw UserError("неизвестный вид номенклатуры: " + std::string(text));
}

Unit unit_from_string(std::string_view text) {
    for (auto const &[u, s] : UNITS) {
        if (s == text) {
            return u;
        }
    }
    throw UserError("неизвестная единица измерения: " + std::string(text));
}

Catalog::Catalog(Database &db) : m_db{db} {}

std::vector<Item> Catalog::list(ItemFilter const &filter) const {
    std::string sql = SELECT_ITEM;
    sql +=
        "WHERE (?1 IS NULL OR i.kind = ?1) AND (?2 OR i.archived = 0) "
        "AND (?3 IS NULL OR i.group_id IN (WITH RECURSIVE sub(id) AS (SELECT ?3 UNION SELECT g.id FROM groups g JOIN sub ON g.parent_id = sub.id) "
        "SELECT id FROM sub)) ORDER BY "
        "CASE i.kind WHEN 'product' THEN 0 WHEN 'semi' THEN 1 ELSE 2 END, i.name";
    auto stmt = m_db.prepare(sql);
    if (filter.kind) {
        stmt.bind(1, to_string(*filter.kind));
    }
    stmt.bind(2, static_cast<std::int64_t>(filter.with_archived));
    if (filter.group) {
        stmt.bind(3, *filter.group);
    }
    std::vector<Item> items;
    while (stmt.step()) {
        items.push_back(read_item(stmt));
    }
    return items;
}

Item Catalog::get(std::int64_t id) const {
    auto stmt = m_db.prepare(std::string(SELECT_ITEM) + "WHERE i.id = ?");
    stmt.bind(1, id);
    if (!stmt.step()) {
        throw NotFoundError("нет позиции номенклатуры с id " + std::to_string(id));
    }
    return read_item(stmt);
}

std::int64_t Catalog::create(Item const &item) {
    auto const name = check_name(item.name);
    Transaction tx{m_db};
    auto dup = m_db.prepare("SELECT count(*) FROM items WHERE name = ?");
    dup.bind(1, name);
    dup.step();
    if (dup.column_int(0) != 0) {
        throw UserError("позиция «" + name + "» уже есть");
    }
    check_group(item.group_id);
    check_vat(item.vat_rate);
    auto stmt = m_db.prepare("INSERT INTO items (name, kind, unit, archived, group_id, vat_rate) VALUES (?, ?, ?, ?, ?, ?)");
    stmt.bind(1, name).bind(2, to_string(item.kind)).bind(3, to_string(item.unit)).bind(4, static_cast<std::int64_t>(item.archived));
    bind_group(stmt, 5, item.group_id);
    if (item.vat_rate) {
        stmt.bind(6, *item.vat_rate);
    }
    stmt.step();
    auto const id = m_db.last_insert_id();
    tx.commit();
    return id;
}

void Catalog::update(Item const &item) {
    auto const name = check_name(item.name);
    Transaction tx{m_db};
    auto const old = get(item.id);

    auto dup = m_db.prepare("SELECT count(*) FROM items WHERE name = ? AND id <> ?");
    dup.bind(1, name).bind(2, item.id);
    dup.step();
    if (dup.column_int(0) != 0) {
        throw UserError("позиция «" + name + "» уже есть");
    }
    if (item.kind == Kind::RAW && old.has_recipe) {
        throw UserError("у «" + old.name + "» есть рецептура: сырьём её сделать нельзя, сначала удалите рецептуру");
    }
    if (item.unit != old.unit) {
        if (old.has_recipe) {
            throw UserError("у «" + old.name + "» есть рецептура в единицах " + std::string(to_string(old.unit)) + ": сначала удалите рецептуру");
        }
        auto const used = count(m_db, "SELECT count(*) FROM recipe_lines WHERE component_id = ?", item.id);
        if (used != 0) {
            throw UserError("«" + old.name + "» входит в рецептуры (" + std::to_string(used) + "), единицу менять нельзя");
        }
        if (count(m_db, "SELECT count(*) FROM doc_lines WHERE item_id = ?", item.id) != 0) {
            throw UserError("«" + old.name + "» есть в документах, единицу менять нельзя");
        }
    }

    check_group(item.group_id);
    check_vat(item.vat_rate);
    auto stmt = m_db.prepare("UPDATE items SET name = ?, kind = ?, unit = ?, archived = ?, group_id = ?, vat_rate = ? WHERE id = ?");
    stmt.bind(1, name).bind(2, to_string(item.kind)).bind(3, to_string(item.unit)).bind(4, static_cast<std::int64_t>(item.archived));
    bind_group(stmt, 5, item.group_id);
    if (item.vat_rate) {
        stmt.bind(6, *item.vat_rate);
    } else {
        stmt.bind_null(6);
    }
    stmt.bind(7, item.id);
    stmt.step();
    tx.commit();
}

void Catalog::remove(std::int64_t id) {
    Transaction tx{m_db};
    auto const item = get(id);

    std::vector<std::int64_t> parents;
    auto stmt = m_db.prepare("SELECT item_id FROM recipe_lines WHERE component_id = ?");
    stmt.bind(1, id);
    while (stmt.step()) {
        parents.push_back(stmt.column_int(0));
    }
    if (!parents.empty()) {
        throw UserError("«" + item.name + "» входит в рецептуры: " + names_of(parents, ", ") + ". Уберите из них или отправьте в архив");
    }
    if (count(m_db, "SELECT count(*) FROM doc_lines WHERE item_id = ?", id) != 0) {
        throw UserError("«" + item.name + "» есть в документах, удалить нельзя: отправьте в архив");
    }

    auto del = m_db.prepare("DELETE FROM items WHERE id = ?");
    del.bind(1, id);
    del.step();
    tx.commit();
}

std::optional<Recipe> Catalog::recipe(std::int64_t item_id) const {
    auto head = m_db.prepare("SELECT output_qty FROM recipes WHERE item_id = ?");
    head.bind(1, item_id);
    if (!head.step()) {
        return std::nullopt;
    }
    Recipe recipe;
    recipe.output_qty = head.column_int(0);

    auto stmt = m_db.prepare(
        "SELECT l.component_id, i.name, i.kind, i.unit, l.qty FROM recipe_lines l JOIN items i ON i.id = l.component_id "
        "WHERE l.item_id = ? ORDER BY l.rowid");
    stmt.bind(1, item_id);
    while (stmt.step()) {
        RecipeLine line;
        line.component_id = stmt.column_int(0);
        line.name = stmt.column_text(1);
        line.kind = kind_from_string(stmt.column_text(2));
        line.unit = unit_from_string(stmt.column_text(3));
        line.qty = stmt.column_int(4);
        recipe.lines.push_back(std::move(line));
    }
    return recipe;
}

void Catalog::set_recipe(std::int64_t item_id, Recipe const &recipe) {
    Transaction tx{m_db};
    auto const item = get(item_id);
    if (item.kind == Kind::RAW) {
        throw UserError("«" + item.name + "» это сырьё, у сырья нет рецептуры");
    }
    check_qty(recipe.output_qty, "выход");
    if (recipe.lines.empty()) {
        throw UserError("в рецептуре нет ни одного компонента");
    }

    std::set<std::int64_t> old_components;
    if (auto const old = this->recipe(item_id)) {
        for (auto const &l : old->lines) {
            old_components.insert(l.component_id);
        }
    }

    std::set<std::int64_t> seen;
    for (auto const &line : recipe.lines) {
        if (line.component_id == item_id) {
            throw UserError("«" + item.name + "» не может входить в собственную рецептуру");
        }
        auto const component = get(line.component_id);
        if (!seen.insert(line.component_id).second) {
            throw UserError("«" + component.name + "» указан в рецептуре дважды");
        }
        check_qty(line.qty, component.name);
        if (component.archived && !old_components.contains(line.component_id)) {
            throw UserError("«" + component.name + "» в архиве, добавить в рецептуру нельзя");
        }
        auto const path = find_path(line.component_id, item_id);
        if (!path.empty()) {
            auto cycle = path;
            cycle.insert(cycle.begin(), item_id);
            throw UserError("рецептура замкнётся в цикл: " + names_of(cycle, " → "));
        }
    }

    auto del = m_db.prepare("DELETE FROM recipes WHERE item_id = ?");
    del.bind(1, item_id);
    del.step();
    auto head = m_db.prepare("INSERT INTO recipes (item_id, output_qty) VALUES (?, ?)");
    head.bind(1, item_id).bind(2, recipe.output_qty);
    head.step();
    auto ins = m_db.prepare("INSERT INTO recipe_lines (item_id, component_id, qty) VALUES (?, ?, ?)");
    for (auto const &line : recipe.lines) {
        ins.reset();
        ins.bind(1, item_id).bind(2, line.component_id).bind(3, line.qty);
        ins.step();
    }
    tx.commit();
}

void Catalog::remove_recipe(std::int64_t item_id) {
    auto stmt = m_db.prepare("DELETE FROM recipes WHERE item_id = ?");
    stmt.bind(1, item_id);
    stmt.step();
}

void Catalog::check_group(std::optional<std::int64_t> group) const {
    if (group) {
        (void)Groups{m_db}.get(*group);
    }
}

std::vector<std::int64_t> Catalog::find_path(std::int64_t from, std::int64_t to) const {
    // обход в глубину по рецептурам с запоминанием предка для восстановления пути
    std::map<std::int64_t, std::int64_t> parent{{from, from}};
    std::vector<std::int64_t> stack{from};
    auto stmt = m_db.prepare("SELECT component_id FROM recipe_lines WHERE item_id = ?");
    while (!stack.empty()) {
        auto const cur = stack.back();
        stack.pop_back();
        if (cur == to) {
            std::vector<std::int64_t> path{to};
            for (auto id = to; id != from;) {
                id = parent.at(id);
                path.insert(path.begin(), id);
            }
            return path;
        }
        stmt.reset();
        stmt.bind(1, cur);
        while (stmt.step()) {
            auto const next = stmt.column_int(0);
            if (parent.emplace(next, cur).second) {
                stack.push_back(next);
            }
        }
    }
    return {};
}

std::string Catalog::names_of(std::vector<std::int64_t> const &ids, std::string_view separator) const {
    std::string out;
    auto stmt = m_db.prepare("SELECT name FROM items WHERE id = ?");
    for (auto const id : ids) {
        stmt.reset();
        stmt.bind(1, id);
        if (!out.empty()) {
            out += separator;
        }
        out += stmt.step() ? stmt.column_text(0) : "#" + std::to_string(id);
    }
    return out;
}
