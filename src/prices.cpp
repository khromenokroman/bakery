#include "prices.hpp"

#include "errors.hpp"

namespace {

constexpr std::size_t MAX_NAME = 100;

/// Последняя цена каждой позиции по типу ?1 на дату ?2 из проведённых установок цен.
constexpr char const LATEST[] =
    "SELECT item_id, price_kop, date, doc_id FROM ("
    "  SELECT l.item_id, l.price_kop, d.date, d.id AS doc_id,"
    "         row_number() OVER (PARTITION BY l.item_id ORDER BY d.date DESC, d.id DESC) AS rn"
    "  FROM doc_lines l JOIN docs d ON d.id = l.doc_id"
    "  WHERE d.type = 'prices' AND d.status = 'posted' AND d.price_type_id = ?1 AND d.date <= ?2"
    ") WHERE rn = 1";

} // namespace

Prices::Prices(Database &db) : m_db{db} {}

std::vector<PriceType> Prices::types() const {
    auto stmt = m_db.prepare("SELECT id, name, system FROM price_types ORDER BY system DESC, name");
    std::vector<PriceType> out;
    while (stmt.step()) {
        out.push_back({stmt.column_text(1), stmt.column_int(0), stmt.column_int(2) != 0});
    }
    return out;
}

PriceType Prices::type(std::int64_t id) const {
    auto stmt = m_db.prepare("SELECT name, system FROM price_types WHERE id = ?");
    stmt.bind(1, id);
    if (!stmt.step()) {
        throw NotFoundError("нет типа цен с id " + std::to_string(id));
    }
    return {stmt.column_text(0), id, stmt.column_int(1) != 0};
}

std::int64_t Prices::create_type(std::string const &name) {
    Transaction tx{m_db};
    auto const clean = check_name(name, 0);
    auto stmt = m_db.prepare("INSERT INTO price_types (name) VALUES (?)");
    stmt.bind(1, clean);
    stmt.step();
    auto const id = m_db.last_insert_id();
    tx.commit();
    return id;
}

void Prices::rename_type(std::int64_t id, std::string const &name) {
    Transaction tx{m_db};
    if (type(id).system) {
        throw UserError("встроенный тип цен переименовать нельзя");
    }
    auto const clean = check_name(name, id);
    auto stmt = m_db.prepare("UPDATE price_types SET name = ? WHERE id = ?");
    stmt.bind(1, clean).bind(2, id);
    stmt.step();
    tx.commit();
}

void Prices::remove_type(std::int64_t id) {
    Transaction tx{m_db};
    auto const t = type(id);
    if (t.system) {
        throw UserError("встроенный тип цен «" + t.name + "» удалить нельзя");
    }
    auto used = m_db.prepare("SELECT count(*) FROM docs WHERE price_type_id = ?");
    used.bind(1, id);
    used.step();
    if (auto const n = used.column_int(0); n != 0) {
        throw UserError("тип цен «" + t.name + "» используется в документах (" + std::to_string(n) + "), удалить нельзя");
    }
    auto del = m_db.prepare("DELETE FROM price_types WHERE id = ?");
    del.bind(1, id);
    del.step();
    tx.commit();
}

std::optional<std::int64_t> Prices::price(std::int64_t type_id, std::int64_t item_id, std::string const &moment) const {
    // от установок цен назад по времени (индекс docs_type_date), строка позиции - по doc_lines_doc_item:
    // время не зависит от числа других документов и строк позиции
    auto stmt = m_db.prepare(
        "SELECT l.price_kop FROM docs d INDEXED BY docs_type_date CROSS JOIN doc_lines l INDEXED BY doc_lines_doc_item "
        "ON l.doc_id = d.id AND l.item_id = ?3 WHERE d.type = 'prices' AND d.status = 'posted' AND d.price_type_id = ?1 AND d.date <= ?2 "
        "ORDER BY d.date DESC, d.id DESC LIMIT 1");
    // одна дата без времени означает конец дня
    stmt.bind(1, type_id).bind(2, moment.size() == 10 ? moment + " 23:59:59" : moment).bind(3, item_id);
    if (!stmt.step()) {
        return std::nullopt;
    }
    return stmt.column_int(0);
}

std::vector<PriceRow> Prices::price_list(std::int64_t type_id, std::string const &date) const {
    auto stmt = m_db.prepare("SELECT p.item_id, i.name, i.unit, p.price_kop, p.date, p.doc_id FROM (" + std::string(LATEST) +
                             ") p JOIN items i ON i.id = p.item_id ORDER BY i.name");
    stmt.bind(1, type_id).bind(2, date.size() == 10 ? date + " 23:59:59" : date);
    std::vector<PriceRow> out;
    while (stmt.step()) {
        PriceRow row;
        row.item_id = stmt.column_int(0);
        row.name = stmt.column_text(1);
        row.unit = unit_from_string(stmt.column_text(2));
        row.price_kop = stmt.column_int(3);
        row.date = stmt.column_text(4).substr(0, 10);
        row.doc_id = stmt.column_int(5);
        out.push_back(std::move(row));
    }
    return out;
}

std::string Prices::check_name(std::string const &name, std::int64_t self) const {
    auto const first = name.find_first_not_of(" \t\r\n");
    auto const clean = first == std::string::npos ? std::string{} : name.substr(first, name.find_last_not_of(" \t\r\n") - first + 1);
    if (clean.empty()) {
        throw UserError("название типа цен не может быть пустым");
    }
    if (clean.size() > MAX_NAME) {
        throw UserError("название типа цен длиннее " + std::to_string(MAX_NAME) + " байт");
    }
    auto dup = m_db.prepare("SELECT count(*) FROM price_types WHERE name = ? AND id <> ?");
    dup.bind(1, clean).bind(2, self);
    dup.step();
    if (dup.column_int(0) != 0) {
        throw UserError("тип цен «" + clean + "» уже есть");
    }
    return clean;
}
