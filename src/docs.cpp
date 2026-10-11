#include "docs.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <set>
#include <utility>

#include "errors.hpp"
#include "prices.hpp"
#include "stock.hpp"

namespace {

constexpr std::int64_t MAX_QTY = 1'000'000'000;
constexpr std::size_t MAX_COMMENT = 1000;

/// Верхняя граница цены: 100 млн рублей в копейках.
constexpr std::int64_t MAX_PRICE = 10'000'000'000;

constexpr std::array<std::pair<DocType, std::string_view>, 7> TYPES = {{
    {DocType::ORDER, "order"},
    {DocType::PRODUCTION, "production"},
    {DocType::SALE, "sale"},
    {DocType::RETURN, "return"},
    {DocType::WRITEOFF, "writeoff"},
    {DocType::INVENTORY, "inventory"},
    {DocType::PRICES, "prices"},
}};

/// Знак движения склада для вида документа; 0 - вид без движений.
int move_sign(DocType type) { return type == DocType::PRODUCTION ? 1 : 0; }

/// Виды, которые проводятся: со складскими движениями или установка цен.
bool postable(DocType type) { return move_sign(type) != 0 || type == DocType::PRICES; }

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
    auto head = m_db.prepare("SELECT type, number, date, status, comment, price_type_id FROM docs WHERE id = ?");
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
    if (!head.column_is_null(5)) {
        doc.price_type_id = head.column_int(5);
    }

    auto lines = m_db.prepare(
        "SELECT l.item_id, i.name, i.unit, l.qty, l.price_kop, l.cost_kop FROM doc_lines l JOIN items i ON i.id = l.item_id "
        "WHERE l.doc_id = ? ORDER BY l.id");
    lines.bind(1, id);
    while (lines.step()) {
        DocLine line;
        line.item_id = lines.column_int(0);
        line.name = lines.column_text(1);
        line.unit = unit_from_string(lines.column_text(2));
        line.qty = lines.column_int(3);
        if (!lines.column_is_null(4)) {
            line.price_kop = lines.column_int(4);
        }
        if (!lines.column_is_null(5)) {
            line.cost_kop = lines.column_int(5);
        }
        doc.lines.push_back(std::move(line));
    }
    return doc;
}

std::int64_t Documents::create(Document const &input) {
    auto doc = input;
    if (doc.type != DocType::PRICES) {
        doc.price_type_id.reset();
    }
    Transaction tx{m_db};
    validate(doc, {});
    auto next = m_db.prepare("SELECT coalesce(max(number), 0) + 1 FROM docs WHERE type = ?");
    next.bind(1, to_string(doc.type));
    next.step();
    auto const number = next.column_int(0);

    auto ins = m_db.prepare("INSERT INTO docs (type, number, date, comment, price_type_id) VALUES (?, ?, ?, ?, ?)");
    ins.bind(1, to_string(doc.type)).bind(2, number).bind(3, doc.date).bind(4, doc.comment);
    if (doc.price_type_id) {
        ins.bind(5, *doc.price_type_id);
    }
    ins.step();
    auto const id = m_db.last_insert_id();
    auto saved = doc;
    saved.id = id;
    write_lines(saved);
    tx.commit();
    return id;
}

void Documents::update(Document const &doc) {
    Transaction tx{m_db};
    auto const old = get(doc.id);
    auto const old_items = item_ids(old);
    auto checked = doc;
    checked.type = old.type;
    if (checked.type != DocType::PRICES) {
        checked.price_type_id.reset();
    }
    validate(checked, old_items);

    auto upd = m_db.prepare("UPDATE docs SET date = ?, comment = ?, price_type_id = ? WHERE id = ?");
    upd.bind(1, doc.date).bind(2, doc.comment).bind(4, doc.id);
    if (checked.price_type_id) {
        upd.bind(3, *checked.price_type_id);
    } else {
        upd.bind_null(3);
    }
    upd.step();
    auto del = m_db.prepare("DELETE FROM doc_lines WHERE doc_id = ?");
    del.bind(1, doc.id);
    del.step();
    write_lines(checked);

    if (old.status == "posted") {
        // перепроведение: старые движения заменяются новыми, минус проверяется только по итогу
        auto moves = m_db.prepare("DELETE FROM stock_moves WHERE doc_id = ?");
        moves.bind(1, doc.id);
        moves.step();
        write_moves(get(doc.id));
        auto items = old_items;
        auto const fresh = item_ids(checked);
        items.insert(items.end(), fresh.begin(), fresh.end());
        Stock{m_db}.check_non_negative(items);
    }
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
    if (!postable(doc.type)) {
        throw UserError("документ этого вида не проводится");
    }
    write_moves(doc);
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
    if (doc.type != DocType::ORDER && doc.type != DocType::PRODUCTION && doc.type != DocType::PRICES) {
        throw UserError("документы вида " + std::string(to_string(doc.type)) + " пока не поддерживаются");
    }
    auto const prices = doc.type == DocType::PRICES;
    if (prices) {
        if (!doc.price_type_id) {
            throw UserError("не выбран тип цен");
        }
        (void)Prices{m_db}.type(*doc.price_type_id);
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
        if (prices) {
            if (!line.price_kop) {
                throw UserError(item.name + ": не указана цена");
            }
            if (*line.price_kop < 0 || *line.price_kop > MAX_PRICE) {
                throw UserError(item.name + ": цена должна быть от 0 до 100 000 000 рублей");
            }
            continue;
        }
        if (line.qty <= 0) {
            throw UserError(item.name + ": количество должно быть больше нуля");
        }
        if (line.qty > MAX_QTY) {
            throw UserError(item.name + ": количество больше миллиона единиц");
        }
    }
}

void Documents::write_moves(Document const &doc) {
    auto const sign = move_sign(doc.type);
    auto ins = m_db.prepare("INSERT INTO stock_moves (doc_id, item_id, qty, date) VALUES (?, ?, ?, ?)");
    for (auto const &line : doc.lines) {
        ins.reset();
        ins.bind(1, doc.id).bind(2, line.item_id).bind(3, sign * line.qty).bind(4, doc.date);
        ins.step();
    }
}

void Documents::write_lines(Document const &doc) {
    Prices const prices{m_db};
    auto const with_price = doc.type == DocType::PRICES;
    auto const with_cost = move_sign(doc.type) != 0;
    auto ins = m_db.prepare("INSERT INTO doc_lines (doc_id, item_id, qty, price_kop, cost_kop) VALUES (?, ?, ?, ?, ?)");
    for (auto const &line : doc.lines) {
        ins.reset();
        ins.bind(1, doc.id).bind(2, line.item_id).bind(3, with_price ? 0 : line.qty);
        if (with_price && line.price_kop) {
            ins.bind(4, *line.price_kop);
        }
        if (with_cost) {
            if (auto const cost = prices.price(COST_PRICE_TYPE, line.item_id, doc.date)) {
                ins.bind(5, *cost);
            }
        }
        ins.step();
    }
}
