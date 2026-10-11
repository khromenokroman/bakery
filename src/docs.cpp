#include "docs.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <set>
#include <utility>

#include "errors.hpp"
#include "stock.hpp"

namespace {

constexpr std::int64_t MAX_QTY = 1'000'000'000;
constexpr std::size_t MAX_COMMENT = 1000;

constexpr std::array<std::pair<DocType, std::string_view>, 6> TYPES = {{
    {DocType::ORDER, "order"},
    {DocType::PRODUCTION, "production"},
    {DocType::SALE, "sale"},
    {DocType::RETURN, "return"},
    {DocType::WRITEOFF, "writeoff"},
    {DocType::INVENTORY, "inventory"},
}};

/// Знак движения склада для вида документа; 0 - вид без движений.
int move_sign(DocType type) { return type == DocType::PRODUCTION ? 1 : 0; }

std::vector<std::int64_t> item_ids(Document const &doc) {
    std::vector<std::int64_t> ids;
    for (auto const &l : doc.lines) {
        ids.push_back(l.item_id);
    }
    return ids;
}

int parse_part(std::string_view text) {
    int value = 0;
    auto const [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    return ec == std::errc{} && ptr == text.data() + text.size() ? value : -1;
}

} // namespace

std::string_view to_string(DocType type) {
    for (auto const &[t, s] : TYPES) {
        if (t == type) {
            return s;
        }
    }
    return "?";
}

DocType doc_type_from_string(std::string_view text) {
    for (auto const &[t, s] : TYPES) {
        if (s == text) {
            return t;
        }
    }
    throw UserError("неизвестный вид документа: " + std::string(text));
}

void check_date(std::string_view date) {
    auto const fail = [&] { throw UserError("неверная дата «" + std::string(date) + "», нужен формат ГГГГ-ММ-ДД"); };
    if (date.size() != 10 || date[4] != '-' || date[7] != '-') {
        fail();
    }
    auto const y = parse_part(date.substr(0, 4));
    auto const m = parse_part(date.substr(5, 2));
    auto const d = parse_part(date.substr(8, 2));
    if (y < 2000 || m < 0 || d < 0) {
        fail();
    }
    std::chrono::year_month_day const ymd{std::chrono::year{y}, std::chrono::month{static_cast<unsigned>(m)},
                                          std::chrono::day{static_cast<unsigned>(d)}};
    if (!ymd.ok()) {
        fail();
    }
}

Documents::Documents(Database &db) : m_db{db} {}

std::vector<Document> Documents::list(DocFilter const &filter) const {
    // виды передаются строкой через запятую и ищутся в ней как ",order,"
    std::string types;
    for (auto const t : filter.types) {
        types += "," + std::string(to_string(t));
    }
    auto stmt = m_db.prepare(
        "SELECT id FROM docs WHERE (?1 IS NULL OR instr(?1 || ',', ',' || type || ',') > 0) AND (?2 IS NULL OR date >= ?2) "
        "AND (?3 IS NULL OR date <= ?3) ORDER BY date, type, number");
    if (!types.empty()) {
        stmt.bind(1, types);
    }
    if (filter.date_from) {
        stmt.bind(2, *filter.date_from);
    }
    if (filter.date_to) {
        stmt.bind(3, *filter.date_to);
    }
    std::vector<std::int64_t> ids;
    while (stmt.step()) {
        ids.push_back(stmt.column_int(0));
    }
    std::vector<Document> docs;
    docs.reserve(ids.size());
    for (auto const id : ids) {
        docs.push_back(get(id));
    }
    return docs;
}

Document Documents::get(std::int64_t id) const {
    auto head = m_db.prepare("SELECT type, number, date, status, comment FROM docs WHERE id = ?");
    head.bind(1, id);
    if (!head.step()) {
        throw NotFoundError("нет документа с id " + std::to_string(id));
    }
    Document doc;
    doc.id = id;
    doc.type = doc_type_from_string(head.column_text(0));
    doc.number = head.column_int(1);
    doc.date = head.column_text(2);
    doc.status = head.column_text(3);
    doc.comment = head.column_text(4);

    auto lines =
        m_db.prepare("SELECT l.item_id, i.name, i.unit, l.qty FROM doc_lines l JOIN items i ON i.id = l.item_id WHERE l.doc_id = ? ORDER BY l.id");
    lines.bind(1, id);
    while (lines.step()) {
        DocLine line;
        line.item_id = lines.column_int(0);
        line.name = lines.column_text(1);
        line.unit = unit_from_string(lines.column_text(2));
        line.qty = lines.column_int(3);
        doc.lines.push_back(std::move(line));
    }
    return doc;
}

std::int64_t Documents::create(Document const &doc) {
    Transaction tx{m_db};
    validate(doc, {});
    auto next = m_db.prepare("SELECT coalesce(max(number), 0) + 1 FROM docs WHERE type = ?");
    next.bind(1, to_string(doc.type));
    next.step();
    auto const number = next.column_int(0);

    auto ins = m_db.prepare("INSERT INTO docs (type, number, date, comment) VALUES (?, ?, ?, ?)");
    ins.bind(1, to_string(doc.type)).bind(2, number).bind(3, doc.date).bind(4, doc.comment);
    ins.step();
    auto const id = m_db.last_insert_id();
    write_lines(id, doc.lines);
    tx.commit();
    return id;
}

void Documents::update(Document const &doc) {
    Transaction tx{m_db};
    auto const old = get(doc.id);
    if (old.status != "draft") {
        throw UserError("документ проведён, изменить его нельзя");
    }
    std::vector<std::int64_t> old_items;
    for (auto const &l : old.lines) {
        old_items.push_back(l.item_id);
    }
    auto checked = doc;
    checked.type = old.type;
    validate(checked, old_items);

    auto upd = m_db.prepare("UPDATE docs SET date = ?, comment = ? WHERE id = ?");
    upd.bind(1, doc.date).bind(2, doc.comment).bind(3, doc.id);
    upd.step();
    auto del = m_db.prepare("DELETE FROM doc_lines WHERE doc_id = ?");
    del.bind(1, doc.id);
    del.step();
    write_lines(doc.id, doc.lines);
    tx.commit();
}

void Documents::remove(std::int64_t id) {
    Transaction tx{m_db};
    if (get(id).status != "draft") {
        throw UserError("документ проведён, удалить его нельзя");
    }
    auto del = m_db.prepare("DELETE FROM docs WHERE id = ?");
    del.bind(1, id);
    del.step();
    tx.commit();
}

void Documents::post(std::int64_t id) {
    Transaction tx{m_db};
    auto const doc = get(id);
    if (doc.status == "posted") {
        throw UserError("документ уже проведён");
    }
    auto const sign = move_sign(doc.type);
    if (sign == 0) {
        throw UserError("документ этого вида не проводится");
    }
    auto ins = m_db.prepare("INSERT INTO stock_moves (doc_id, item_id, qty, date) VALUES (?, ?, ?, ?)");
    for (auto const &line : doc.lines) {
        ins.reset();
        ins.bind(1, id).bind(2, line.item_id).bind(3, sign * line.qty).bind(4, doc.date);
        ins.step();
    }
    auto upd = m_db.prepare("UPDATE docs SET status = 'posted' WHERE id = ?");
    upd.bind(1, id);
    upd.step();
    Stock{m_db}.check_non_negative(item_ids(doc));
    tx.commit();
}

void Documents::unpost(std::int64_t id) {
    Transaction tx{m_db};
    auto const doc = get(id);
    if (doc.status != "posted") {
        throw UserError("документ не проведён");
    }
    auto del = m_db.prepare("DELETE FROM stock_moves WHERE doc_id = ?");
    del.bind(1, id);
    del.step();
    auto upd = m_db.prepare("UPDATE docs SET status = 'draft' WHERE id = ?");
    upd.bind(1, id);
    upd.step();
    Stock{m_db}.check_non_negative(item_ids(doc));
    tx.commit();
}

void Documents::validate(Document const &doc, std::vector<std::int64_t> const &old_items) const {
    if (doc.type != DocType::ORDER && doc.type != DocType::PRODUCTION) {
        throw UserError("документы вида " + std::string(to_string(doc.type)) + " пока не поддерживаются");
    }
    check_date(doc.date);
    if (doc.comment.size() > MAX_COMMENT) {
        throw UserError("комментарий длиннее " + std::to_string(MAX_COMMENT) + " байт");
    }
    if (doc.lines.empty()) {
        throw UserError("в документе нет ни одной строки");
    }
    Catalog const catalog{m_db};
    std::set<std::int64_t> seen;
    for (auto const &line : doc.lines) {
        auto const item = catalog.get(line.item_id);
        if (!seen.insert(line.item_id).second) {
            throw UserError("«" + item.name + "» указан в документе дважды");
        }
        if (item.kind != Kind::PRODUCT) {
            throw UserError("«" + item.name + "» не продукция: в документ входит только продукция");
        }
        if (item.archived && std::find(old_items.begin(), old_items.end(), line.item_id) == old_items.end()) {
            throw UserError("«" + item.name + "» в архиве, добавить в документ нельзя");
        }
        if (line.qty <= 0) {
            throw UserError(item.name + ": количество должно быть больше нуля");
        }
        if (line.qty > MAX_QTY) {
            throw UserError(item.name + ": количество больше миллиона единиц");
        }
    }
}

void Documents::write_lines(std::int64_t doc_id, std::vector<DocLine> const &lines) {
    auto ins = m_db.prepare("INSERT INTO doc_lines (doc_id, item_id, qty) VALUES (?, ?, ?)");
    for (auto const &line : lines) {
        ins.reset();
        ins.bind(1, doc_id).bind(2, line.item_id).bind(3, line.qty);
        ins.step();
    }
}
