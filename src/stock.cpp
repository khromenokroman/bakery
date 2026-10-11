#include "stock.hpp"

#include "errors.hpp"

namespace {

std::string_view unit_label(Unit unit) {
    switch (unit) {
        case Unit::KG:
            return "кг";
        case Unit::L:
            return "л";
        case Unit::PCS:
            return "шт";
    }
    return "?";
}

/// "2026-10-12" -> "12.10.2026".
std::string ru_date(std::string const &iso) { return iso.size() == 10 ? iso.substr(8, 2) + "." + iso.substr(5, 2) + "." + iso.substr(0, 4) : iso; }

} // namespace

std::string format_qty(std::int64_t milli, Unit unit) {
    auto const negative = milli < 0;
    auto const abs = negative ? -milli : milli;
    auto text = std::to_string(abs / 1000);
    if (auto frac = abs % 1000; frac != 0) {
        auto digits = std::to_string(frac + 1000).substr(1);
        digits.erase(digits.find_last_not_of('0') + 1);
        text += "," + digits;
    }
    return (negative ? "-" : "") + text + " " + std::string(unit_label(unit));
}

Stock::Stock(Database &db) : m_db{db} {}

std::vector<Balance> Stock::balances(std::optional<std::string> const &date, std::optional<std::int64_t> group) const {
    auto stmt = m_db.prepare(
        "SELECT m.item_id, i.name, i.unit, i.archived, sum(m.qty), i.group_id FROM stock_moves m JOIN items i ON i.id = m.item_id "
        "WHERE (?1 IS NULL OR m.date <= ?1) AND (?2 IS NULL OR i.group_id IN (WITH RECURSIVE sub(id) AS (SELECT ?2 UNION SELECT g.id FROM groups g "
        "JOIN sub ON g.parent_id = sub.id) SELECT id FROM sub)) "
        "GROUP BY m.item_id HAVING sum(m.qty) <> 0 ORDER BY i.name");
    if (date) {
        stmt.bind(1, *date);
    }
    if (group) {
        stmt.bind(2, *group);
    }
    std::vector<Balance> out;
    while (stmt.step()) {
        Balance b;
        b.item_id = stmt.column_int(0);
        b.name = stmt.column_text(1);
        b.unit = unit_from_string(stmt.column_text(2));
        b.archived = stmt.column_int(3) != 0;
        b.qty = stmt.column_int(4);
        if (!stmt.column_is_null(5)) {
            b.group_id = stmt.column_int(5);
        }
        out.push_back(std::move(b));
    }
    return out;
}

void Stock::check_non_negative(std::vector<std::int64_t> const &items) const {
    // остаток на конец каждого дня: движения внутри дня не упорядочены, важен итог дня
    auto stmt = m_db.prepare("SELECT date, sum(qty) FROM stock_moves WHERE item_id = ? GROUP BY date ORDER BY date");
    for (auto const id : items) {
        stmt.reset();
        stmt.bind(1, id);
        std::int64_t balance = 0;
        while (stmt.step()) {
            balance += stmt.column_int(1);
            if (balance < 0) {
                auto const item = Catalog{m_db}.get(id);
                throw UserError("«" + item.name + "»: остаток на " + ru_date(stmt.column_text(0)) + " уйдёт в минус, не хватает " +
                                format_qty(-balance, item.unit));
            }
        }
    }
}
