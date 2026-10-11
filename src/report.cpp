#include "report.hpp"

#include <map>
#include <utility>

#include "errors.hpp"

namespace {

/// Позиции группы ?3 и её подгрупп (или все, если ?3 пусто).
constexpr char const IN_GROUP[] =
    "(?3 IS NULL OR i.group_id IN (WITH RECURSIVE sub(id) AS (SELECT ?3 UNION SELECT g.id FROM groups g "
    "JOIN sub ON g.parent_id = sub.id) SELECT id FROM sub))";

/// Цена типа ?4 на момент движения m: последняя проведённая установка не позже движения.
constexpr char const PRICE_AT_MOVE[] =
    "(SELECT pl.price_kop FROM doc_lines pl JOIN docs pd ON pd.id = pl.doc_id WHERE pd.type = 'prices' "
    "AND pd.status = 'posted' AND pd.price_type_id = ?4 AND pl.item_id = m.item_id AND pd.date <= m.date "
    "ORDER BY pd.date DESC, pd.id DESC LIMIT 1)";

void bind_group(Statement &stmt, std::optional<std::int64_t> group) {
    if (group) {
        stmt.bind(3, *group);
    }
}

/**
 * @brief Оценка остатка до момента begin по типу цен: каждое движение по цене на его момент.
 *
 * Цены меняются редко, поэтому вместо цены на каждое движение берётся количество движений в каждом
 * интервале действия цены (сумма по покрывающему индексу) и умножается на цену интервала.
 * @return {сумма количество × цена (тысячные × копейки), число движений без цены}
 */
std::pair<std::int64_t, std::int64_t> opening_by_price(Database &db, std::string const &begin, std::optional<std::int64_t> group,
                                                       std::int64_t price_type) {
    // история цен типа до начала периода: по позиции моменты и цены, при равном моменте - последний документ
    std::map<std::int64_t, std::vector<std::pair<std::string, std::int64_t>>> history;
    auto hist = db.prepare(
        "SELECT l.item_id, d.date, l.price_kop FROM doc_lines l JOIN docs d ON d.id = l.doc_id "
        "WHERE d.type = 'prices' AND d.status = 'posted' AND d.price_type_id = ?1 AND d.date < ?2 ORDER BY l.item_id, d.date, d.id");
    hist.bind(1, price_type).bind(2, begin);
    while (hist.step()) {
        auto &list = history[hist.column_int(0)];
        auto const when = hist.column_text(1);
        if (!list.empty() && list.back().first == when) {
            list.back().second = hist.column_int(2);
        } else {
            list.emplace_back(when, hist.column_int(2));
        }
    }

    auto items = db.prepare(std::string("SELECT i.id FROM items i WHERE i.kind = 'product' AND ?1 IS NOT NULL AND ") + IN_GROUP);
    items.bind(1, 1);
    bind_group(items, group);
    // количество и число движений позиции в интервале моментов [from, to)
    auto range = db.prepare("SELECT coalesce(sum(qty), 0), count(*) FROM stock_moves WHERE item_id = ?1 AND date >= ?2 AND date < ?3");
    auto const between = [&](std::int64_t item, std::string const &from, std::string const &to) {
        range.reset();
        range.bind(1, item).bind(2, from).bind(3, to);
        range.step();
        return std::pair{range.column_int(0), range.column_int(1)};
    };

    std::int64_t value = 0;
    std::int64_t without = 0;
    while (items.step()) {
        auto const item = items.column_int(0);
        auto const it = history.find(item);
        if (it == history.end()) {
            without += between(item, "", begin).second;
            continue;
        }
        auto const &list = it->second;
        without += between(item, "", list.front().first).second;
        for (std::size_t k = 0; k < list.size(); ++k) {
            auto const &end = k + 1 < list.size() ? list[k + 1].first : begin;
            value += between(item, list[k].first, end).first * list[k].second;
        }
    }
    return {value, without};
}

} // namespace

GoodsReport goods_report(Database &db, std::string const &from, std::string const &to, std::optional<std::int64_t> group,
                         std::optional<std::int64_t> price_type) {
    check_date(from);
    check_date(to);
    if (from > to) {
        throw UserError("начало периода позже конца");
    }
    auto const begin = from + " 00:00:00";
    auto const end = to + " 23:59:59";
    GoodsReport report;

    // по позициям: количество и стоимость до начала, приход и расход за период, стоимость на конец
    auto items = db.prepare(std::string("SELECT i.id, i.name, i.unit,"
                                        " coalesce(sum(CASE WHEN m.date < ?1 THEN m.qty END), 0),"
                                        " coalesce(sum(CASE WHEN m.date >= ?1 AND m.qty > 0 THEN m.qty END), 0),"
                                        " coalesce(sum(CASE WHEN m.date >= ?1 AND m.qty < 0 THEN -m.qty END), 0),"
                                        " coalesce(sum(CASE WHEN m.date < ?1 THEN m.qty * coalesce(m.cost_kop, 0) END), 0),"
                                        " coalesce(sum(m.qty * coalesce(m.cost_kop, 0)), 0),"
                                        " coalesce(sum(CASE WHEN m.cost_kop IS NULL THEN 1 END), 0)"
                                        " FROM stock_moves m INDEXED BY stock_moves_cover JOIN items i ON i.id = m.item_id"
                                        " WHERE m.date <= ?2 AND ") +
                            IN_GROUP + " GROUP BY i.id ORDER BY i.name");
    items.bind(1, begin).bind(2, end);
    bind_group(items, group);
    while (items.step()) {
        ReportItem r;
        r.item_id = items.column_int(0);
        r.name = items.column_text(1);
        r.unit = unit_from_string(items.column_text(2));
        r.opening = items.column_int(3);
        r.in = items.column_int(4);
        r.out = items.column_int(5);
        r.closing = r.opening + r.in - r.out;
        // произведение количества (тысячные) на цену (копейки) делится на 1000 с округлением
        r.opening_sum = (items.column_int(6) + 500) / 1000;
        r.closing_sum = (items.column_int(7) + 500) / 1000;
        report.lines_without_cost += items.column_int(8);
        if (r.opening != 0 || r.in != 0 || r.out != 0) {
            report.opening_sum += r.opening_sum;
            report.items.push_back(std::move(r));
        }
    }

    // вторая оценка по типу цен: без типа выражение цены - NULL и не вычисляется
    std::string const price = price_type ? PRICE_AT_MOVE : "NULL";
    report.price_type_id = price_type;
    if (price_type) {
        auto const [value, without] = opening_by_price(db, begin, group, *price_type);
        report.opening_price_sum = (value + 500) / 1000;
        report.moves_without_price = without;
    }

    // документы периода с суммами по себестоимости и по типу цен
    auto docs = db.prepare(
        "SELECT d.id, d.type, d.number, d.date, coalesce(p.name, ''), sum(m.qty * coalesce(m.cost_kop, 0)), m.qty > 0, "
        "coalesce(sum(m.qty * coalesce(" +
        price + ", 0)), 0), coalesce(sum(" + price +
        " IS NULL), 0)"
        " FROM stock_moves m JOIN docs d ON d.id = m.doc_id JOIN items i ON i.id = m.item_id"
        " LEFT JOIN partners p ON p.id = d.partner_id"
        " WHERE m.date >= ?1 AND m.date <= ?2 AND " +
        std::string(IN_GROUP) + " GROUP BY d.id, m.qty > 0 ORDER BY d.date, d.id");
    docs.bind(1, begin).bind(2, end);
    bind_group(docs, group);
    if (price_type) {
        docs.bind(4, *price_type);
    }
    while (docs.step()) {
        ReportDoc d;
        d.id = docs.column_int(0);
        d.type = doc_type_from_string(docs.column_text(1));
        d.number = docs.column_int(2);
        auto const when = docs.column_text(3);
        d.date = when.substr(0, 10);
        d.time = when.size() > 11 ? when.substr(11) : "00:00:00";
        d.partner_name = docs.column_text(4);
        auto const value = docs.column_int(5);
        // приход - положительные движения (поступление, возврат, излишки инвентаризации) по знаку количества:
        // сумма бывает нулевой, если себестоимости нет
        d.incoming = docs.column_int(6) != 0;
        d.sum = ((d.incoming ? value : -value) + 500) / 1000;
        auto const price_value = docs.column_int(7);
        d.price_sum = ((d.incoming ? price_value : -price_value) + 500) / 1000;
        if (price_type) {
            report.moves_without_price += docs.column_int(8);
        }
        if (d.incoming) {
            report.in_sum += d.sum;
            report.in_price_sum += d.price_sum;
            report.incoming.push_back(std::move(d));
        } else {
            report.out_sum += d.sum;
            report.out_price_sum += d.price_sum;
            report.outgoing.push_back(std::move(d));
        }
    }
    // остаток на конец по арифметике отчёта, чтобы начало + приход - расход сходилось до копейки
    report.closing_sum = report.opening_sum + report.in_sum - report.out_sum;
    report.closing_price_sum = report.opening_price_sum + report.in_price_sum - report.out_price_sum;
    return report;
}
