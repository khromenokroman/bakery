#include "docs.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <map>
#include <set>
#include <utility>

#include "errors.hpp"
#include "partners.hpp"
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
int move_sign(DocType type) {
    switch (type) {
        case DocType::PRODUCTION:
        case DocType::RETURN:
            return 1;
        case DocType::SALE:
        case DocType::WRITEOFF:
            return -1;
        default:
            return 0;
    }
}

bool has_price_type(DocType type) { return type == DocType::PRICES || type == DocType::SALE || type == DocType::RETURN; }
bool has_partner(DocType type) { return type == DocType::ORDER || type == DocType::SALE || type == DocType::RETURN; }

/// Убирает из шапки поля, которых у вида документа нет.
void normalize(Document &doc) {
    if (!has_price_type(doc.type)) {
        doc.price_type_id.reset();
    }
    if (!has_partner(doc.type)) {
        doc.partner_id.reset();
    }
    if (doc.type != DocType::RETURN) {
        doc.base_id.reset();
    }
}

/// Сумма количества по позициям документа.
std::map<std::int64_t, std::int64_t> qty_by_item(Document const &doc) {
    std::map<std::int64_t, std::int64_t> out;
    for (auto const &l : doc.lines) {
        out[l.item_id] += l.qty;
    }
    return out;
}

/// "2026-10-12" -> "12.10.2026".
std::string ru_date(std::string const &iso) { return iso.size() == 10 ? iso.substr(8, 2) + "." + iso.substr(5, 2) + "." + iso.substr(0, 4) : iso; }

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
    auto head = m_db.prepare(
        "SELECT d.type, d.number, d.date, d.status, d.comment, d.price_type_id, d.partner_id, p.name, d.base_id FROM docs d "
        "LEFT JOIN partners p ON p.id = d.partner_id WHERE d.id = ?");
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
    if (!head.column_is_null(6)) {
        doc.partner_id = head.column_int(6);
        doc.partner_name = head.column_text(7);
    }
    if (!head.column_is_null(8)) {
        doc.base_id = head.column_int(8);
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
    normalize(doc);
    Transaction tx{m_db};
    if (doc.type == DocType::RETURN && doc.base_id) {
        auto const sale = get(*doc.base_id);
        doc.partner_id = sale.partner_id;
        doc.price_type_id = sale.price_type_id;
    }
    validate(doc, {});
    if (doc.partner_id) {
        auto const partner = Partners{m_db}.get(*doc.partner_id);
        if (partner.archived) {
            throw UserError("клиент «" + partner.name + "» в архиве, выбрать его нельзя");
        }
    }
    auto next = m_db.prepare("SELECT coalesce(max(number), 0) + 1 FROM docs WHERE type = ?");
    next.bind(1, to_string(doc.type));
    next.step();
    auto const number = next.column_int(0);

    auto ins = m_db.prepare("INSERT INTO docs (type, number, date, comment, price_type_id, partner_id, base_id) VALUES (?, ?, ?, ?, ?, ?, ?)");
    ins.bind(1, to_string(doc.type)).bind(2, number).bind(3, doc.date).bind(4, doc.comment);
    if (doc.price_type_id) {
        ins.bind(5, *doc.price_type_id);
    }
    if (doc.partner_id) {
        ins.bind(6, *doc.partner_id);
    }
    if (doc.base_id) {
        ins.bind(7, *doc.base_id);
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
    checked.base_id = old.base_id;
    normalize(checked);
    if (checked.type == DocType::RETURN && checked.base_id) {
        auto const sale = get(*checked.base_id);
        checked.partner_id = sale.partner_id;
        checked.price_type_id = sale.price_type_id;
    }
    validate(checked, old_items);
    if (checked.partner_id && checked.partner_id != old.partner_id) {
        auto const partner = Partners{m_db}.get(*checked.partner_id);
        if (partner.archived) {
            throw UserError("клиент «" + partner.name + "» в архиве, выбрать его нельзя");
        }
    }

    auto upd = m_db.prepare("UPDATE docs SET date = ?, comment = ?, price_type_id = ?, partner_id = ? WHERE id = ?");
    upd.bind(1, doc.date).bind(2, doc.comment).bind(5, doc.id);
    if (checked.price_type_id) {
        upd.bind(3, *checked.price_type_id);
    } else {
        upd.bind_null(3);
    }
    if (checked.partner_id) {
        upd.bind(4, *checked.partner_id);
    } else {
        upd.bind_null(4);
    }
    upd.step();
    auto del = m_db.prepare("DELETE FROM doc_lines WHERE doc_id = ?");
    del.bind(1, doc.id);
    del.step();
    write_lines(checked);

    if (old.status == "posted" && old.type == DocType::SALE) {
        check_sale_returns(doc.id, checked);
    }
    if (old.status == "posted") {
        // перепроведение: старые движения заменяются новыми, минус проверяется только по итогу
        auto moves = m_db.prepare("DELETE FROM stock_moves WHERE doc_id = ?");
        moves.bind(1, doc.id);
        moves.step();
        auto const fresh_doc = get(doc.id);
        check_postable(fresh_doc);
        write_moves(fresh_doc);
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
    auto based = m_db.prepare("SELECT group_concat(number, ', ') FROM docs WHERE base_id = ?");
    based.bind(1, id);
    if (based.step() && !based.column_is_null(0)) {
        throw UserError("по документу оформлены возвраты № " + based.column_text(0) + ", сначала удалите их");
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
    check_postable(doc);
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
    if (doc.type == DocType::SALE) {
        check_sale_returns(id, std::nullopt);
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
    if (doc.type == DocType::INVENTORY) {
        throw UserError("документы вида " + std::string(to_string(doc.type)) + " пока не поддерживаются");
    }
    if (doc.type == DocType::WRITEOFF && doc.comment.find_first_not_of(" \t\r\n") == std::string::npos) {
        throw UserError("укажите причину списания");
    }
    auto const prices = doc.type == DocType::PRICES;
    if (has_price_type(doc.type) && doc.type != DocType::RETURN) {
        if (!doc.price_type_id) {
            throw UserError("не выбран тип цен");
        }
        (void)Prices{m_db}.type(*doc.price_type_id);
    }
    if (doc.partner_id) {
        (void)Partners{m_db}.get(*doc.partner_id);
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
        if (line.price_kop && (*line.price_kop < 0 || *line.price_kop > MAX_PRICE)) {
            throw UserError(item.name + ": цена должна быть от 0 до 100 000 000 рублей");
        }
        if (line.qty <= 0) {
            throw UserError(item.name + ": количество должно быть больше нуля");
        }
        if (line.qty > MAX_QTY) {
            throw UserError(item.name + ": количество больше миллиона единиц");
        }
    }
    if (doc.type == DocType::RETURN) {
        check_return(doc);
    }
}

void Documents::check_postable(Document const &doc) const {
    if (doc.type == DocType::RETURN) {
        check_return(doc);
        return;
    }
    if (doc.type != DocType::SALE) {
        return;
    }
    std::string missing;
    for (auto const &line : doc.lines) {
        if (!line.price_kop) {
            missing += (missing.empty() ? "" : ", ") + line.name;
        }
    }
    if (!missing.empty()) {
        auto const type = Prices{m_db}.type(*doc.price_type_id);
        throw UserError("нет цены в типе «" + type.name + "» на " + ru_date(doc.date) + ": " + missing + ". Установите цену или введите её в строке");
    }
}

std::vector<Returnable> Documents::returnable(std::int64_t sale_id, std::optional<std::int64_t> exclude_return) const {
    auto const sale = get(sale_id);
    if (sale.type != DocType::SALE) {
        throw UserError("документ № " + std::to_string(sale.number) + " не реализация");
    }
    auto returned = m_db.prepare(
        "SELECT coalesce(sum(l.qty), 0) FROM doc_lines l JOIN docs d ON d.id = l.doc_id "
        "WHERE d.base_id = ? AND d.type = 'return' AND d.status = 'posted' AND l.item_id = ? AND d.id <> ?");
    std::vector<Returnable> out;
    for (auto const &line : sale.lines) {
        returned.reset();
        returned.bind(1, sale_id).bind(2, line.item_id).bind(3, exclude_return.value_or(0));
        returned.step();
        Returnable r;
        r.item_id = line.item_id;
        r.name = line.name;
        r.unit = line.unit;
        r.sold = line.qty;
        r.returned = returned.column_int(0);
        r.price_kop = line.price_kop;
        r.cost_kop = line.cost_kop;
        out.push_back(std::move(r));
    }
    return out;
}

void Documents::check_return(Document const &doc) const {
    if (!doc.base_id) {
        throw UserError("не указана реализация, по которой оформляется возврат");
    }
    auto const sale = get(*doc.base_id);
    if (sale.type != DocType::SALE) {
        throw UserError("основанием возврата может быть только реализация");
    }
    auto const sale_name = "реализация № " + std::to_string(sale.number) + " от " + ru_date(sale.date);
    if (sale.status != "posted") {
        throw UserError(sale_name + " не проведена");
    }
    if (doc.date < sale.date) {
        throw UserError("возврат не может быть раньше реализации: " + sale_name);
    }
    std::map<std::int64_t, Returnable> available;
    for (auto const &r : returnable(sale.id, doc.id ? std::optional{doc.id} : std::nullopt)) {
        available.emplace(r.item_id, r);
    }
    for (auto const &[item, qty] : qty_by_item(doc)) {
        auto const it = available.find(item);
        if (it == available.end()) {
            throw UserError("«" + Catalog{m_db}.get(item).name + "» не было в: " + sale_name);
        }
        auto const &r = it->second;
        if (qty > r.sold - r.returned) {
            throw UserError("«" + r.name + "»: можно вернуть не больше " + format_qty(r.sold - r.returned, r.unit) + " (продано " +
                            format_qty(r.sold, r.unit) + ", уже возвращено " + format_qty(r.returned, r.unit) + ")");
        }
    }
}

void Documents::check_sale_returns(std::int64_t sale_id, std::optional<Document> const &sale) const {
    auto stmt = m_db.prepare(
        "SELECT l.item_id, sum(l.qty), group_concat(DISTINCT d.number) FROM doc_lines l JOIN docs d ON d.id = l.doc_id "
        "WHERE d.base_id = ? AND d.type = 'return' AND d.status = 'posted' GROUP BY l.item_id");
    stmt.bind(1, sale_id);
    std::map<std::int64_t, std::int64_t> returned;
    std::set<std::string> numbers;
    while (stmt.step()) {
        returned[stmt.column_int(0)] = stmt.column_int(1);
        numbers.insert(stmt.column_text(2));
    }
    if (returned.empty()) {
        return;
    }
    if (!sale) {
        std::string list;
        for (auto const &n : numbers) {
            list += (list.empty() ? "" : ", ") + n;
        }
        throw UserError("по реализации проведены возвраты № " + list + ", сначала отмените их проведение");
    }
    auto const sold = qty_by_item(*sale);
    for (auto const &[item, qty] : returned) {
        auto const it = sold.find(item);
        auto const left = it == sold.end() ? 0 : it->second;
        if (left < qty) {
            auto const i = Catalog{m_db}.get(item);
            throw UserError("«" + i.name + "»: по возвратам возвращено " + format_qty(qty, i.unit) + ", а в реализации останется " +
                            format_qty(left, i.unit));
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
    std::map<std::int64_t, Returnable> from_sale;
    if (doc.type == DocType::RETURN && doc.base_id) {
        for (auto const &r : returnable(*doc.base_id)) {
            from_sale.emplace(r.item_id, r);
        }
    }
    auto const with_price = has_price_type(doc.type);
    auto const with_cost = move_sign(doc.type) != 0;
    auto ins = m_db.prepare("INSERT INTO doc_lines (doc_id, item_id, qty, price_kop, cost_kop) VALUES (?, ?, ?, ?, ?)");
    for (auto const &line : doc.lines) {
        ins.reset();
        ins.bind(1, doc.id).bind(2, line.item_id).bind(3, doc.type == DocType::PRICES ? 0 : line.qty);
        if (doc.type == DocType::RETURN) {
            // возврат по ценам и себестоимости реализации
            auto const &src = from_sale.at(line.item_id);
            if (src.price_kop) {
                ins.bind(4, *src.price_kop);
            }
            if (src.cost_kop) {
                ins.bind(5, *src.cost_kop);
            }
            ins.step();
            continue;
        }
        if (with_price && line.price_kop) {
            ins.bind(4, *line.price_kop);
        } else if (doc.type == DocType::SALE && doc.price_type_id) {
            // цена не передана: берётся из типа цен документа на его дату
            if (auto const price = prices.price(*doc.price_type_id, line.item_id, doc.date)) {
                ins.bind(4, *price);
            }
        }
        if (with_cost) {
            if (auto const cost = prices.price(COST_PRICE_TYPE, line.item_id, doc.date)) {
                ins.bind(5, *cost);
            }
        }
        ins.step();
    }
}
