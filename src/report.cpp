#include "report.hpp"

#include <algorithm>
#include <map>
#include <set>
#include <tuple>
#include <utility>

#include "errors.hpp"
#include "prices.hpp"

namespace {

/// Позиции группы ?3 и её подгрупп (или все, если ?3 пусто).
constexpr char const IN_GROUP[] =
    "(?3 IS NULL OR i.group_id IN (WITH RECURSIVE sub(id) AS (SELECT ?3 UNION SELECT g.id FROM groups g "
    "JOIN sub ON g.parent_id = sub.id) SELECT id FROM sub))";

/// Цена типа ?4 на момент движения m: последняя проведённая установка не позже движения.
/// От установок цен назад по времени, строка позиции по индексу (doc_id, item_id): не перебирает все строки позиции.
constexpr char const PRICE_AT_MOVE[] =
    "(SELECT pl.price_kop FROM docs pd INDEXED BY docs_type_date CROSS JOIN doc_lines pl INDEXED BY doc_lines_doc_item "
    "ON pl.doc_id = pd.id AND pl.item_id = m.item_id WHERE pd.type = 'prices' AND pd.status = 'posted' AND pd.price_type_id = ?4 "
    "AND pd.date <= m.date ORDER BY pd.date DESC, pd.id DESC LIMIT 1)";

/// Цена в строке документа движения m (для реализации - цена продажи).
constexpr char const LINE_PRICE[] = "(SELECT l.price_kop FROM doc_lines l WHERE l.doc_id = m.doc_id AND l.item_id = m.item_id LIMIT 1)";

/// Движение реализации, у которого есть и цена продажи, и цена типа: оценивается по цене продажи.
constexpr char const SOLD_AT_LINE[] = "d.type = 'sale' AND x.lp IS NOT NULL AND x.tp IS NOT NULL";

/// Тысячные доли копейки -> копейки, половина от нуля: деление в C++ отбрасывает дробь к нулю.
std::int64_t to_kop(std::int64_t milli) { return (milli >= 0 ? milli + 500 : milli - 500) / 1000; }

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

/// Переоценки по установкам цен: до начала периода - суммой, в периоде - по документам.
struct Revaluation {
    std::map<std::int64_t, std::pair<std::int64_t, std::int64_t>> by_doc; ///< документ -> {по себестоимости, по типу цен}
    std::map<std::int64_t, std::pair<std::int64_t, std::int64_t>> by_item; ///< позиция -> {до периода, до конца} по себестоимости
    std::int64_t before_price{};
};

/**
 * @brief Переоценка остатка при каждой установке цен типа до конца периода.
 *
 * Для позиции установки: остаток до её момента (движения строго раньше; движения в тот же момент уже
 * оценены по новой цене) × (новая цена - прежняя, 0 если цены не было). Количество накапливается по
 * интервалам между установками, каждое движение читается один раз. Суммы - тысячные × копейки.
 * @param price_column записать в колонку типа цен (иначе - себестоимости)
 */
void add_revaluations(Database &db, std::int64_t type, std::string const &begin, std::string const &end, std::optional<std::int64_t> group,
                      bool price_column, Revaluation &out) {
    std::set<std::int64_t> scope;
    auto items = db.prepare(std::string("SELECT i.id FROM items i WHERE i.kind = 'product' AND ?1 IS NOT NULL AND ") + IN_GROUP);
    items.bind(1, 1);
    bind_group(items, group);
    while (items.step()) {
        scope.insert(items.column_int(0));
    }
    auto hist = db.prepare(
        "SELECT l.item_id, d.date, d.id, l.price_kop FROM doc_lines l JOIN docs d ON d.id = l.doc_id "
        "WHERE d.type = 'prices' AND d.status = 'posted' AND d.price_type_id = ?1 AND d.date <= ?2 ORDER BY l.item_id, d.date, d.id");
    hist.bind(1, type).bind(2, end);
    auto range = db.prepare("SELECT coalesce(sum(qty), 0) FROM stock_moves WHERE item_id = ?1 AND date >= ?2 AND date < ?3");

    std::int64_t item = -1;
    std::int64_t prev = 0;
    std::int64_t qty = 0;
    std::string last;
    while (hist.step()) {
        if (hist.column_int(0) != item) {
            item = hist.column_int(0);
            prev = 0;
            qty = 0;
            last.clear();
        }
        if (!scope.contains(item)) {
            continue;
        }
        auto const when = hist.column_text(1);
        range.reset();
        range.bind(1, item).bind(2, last).bind(3, when);
        range.step();
        qty += range.column_int(0);
        last = when;
        auto const price = hist.column_int(3);
        auto const amount = qty * (price - prev);
        prev = price;
        if (!price_column) {
            auto &sums = out.by_item[item];
            sums.second += amount;
            if (when < begin) {
                sums.first += amount;
            }
        }
        if (when < begin) {
            if (price_column) {
                out.before_price += amount;
            }
        } else {
            auto &row = out.by_doc[hist.column_int(2)];
            (price_column ? row.second : row.first) += amount;
        }
    }
}

} // namespace

SalesReport sales_report(Database &db, std::string const &from, std::string const &to, std::optional<std::int64_t> group) {
    check_date(from);
    check_date(to);
    if (from > to) {
        throw UserError("начало периода позже конца");
    }
    auto const begin = from + " 00:00:00";
    auto const end = to + " 23:59:59";

    auto names = db.prepare("SELECT id, name FROM groups");
    std::map<std::int64_t, std::string> group_names;
    while (names.step()) {
        group_names.emplace(names.column_int(0), names.column_text(1));
    }

    // реализация (+qty) и возврат (-qty): количество, себестоимость и выручка позиции за период;
    // себестоимость и цена - из строк документов (л.п. на дату реализации), движение по знаку вида
    auto items = db.prepare(
        "SELECT i.id, i.name, i.unit, coalesce(i.group_id, 0),"
        " sum(CASE WHEN d.type = 'sale' THEN l.qty ELSE -l.qty END),"
        " coalesce(sum(CASE WHEN d.type = 'sale' THEN l.qty * coalesce(l.cost_kop, 0) ELSE -l.qty * coalesce(l.cost_kop, 0) END), 0),"
        " coalesce(sum(CASE WHEN d.type = 'sale' THEN l.qty * coalesce(l.price_kop, 0) ELSE -l.qty * coalesce(l.price_kop, 0) END), 0),"
        " coalesce(sum(CASE WHEN l.cost_kop IS NULL THEN 1 END), 0),"
        " coalesce(sum(CASE WHEN l.price_kop IS NULL THEN 1 END), 0)"
        " FROM doc_lines l JOIN docs d ON d.id = l.doc_id JOIN items i ON i.id = l.item_id"
        " WHERE d.type IN ('sale', 'return') AND d.status = 'posted' AND d.date >= ?1 AND d.date <= ?2 AND " +
        std::string(IN_GROUP) + " GROUP BY i.id ORDER BY i.name");
    items.bind(1, begin).bind(2, end);
    bind_group(items, group);

    auto const pct = [](std::int64_t a, std::int64_t b) {
        if (b <= 0) {
            return std::int64_t{0};
        }
        auto const sign = a < 0 ? -1 : 1;
        auto const n = a < 0 ? -a : a;
        return sign * static_cast<std::int64_t>((n * 100 + b / 2) / b);
    };

    // накопление по группе товара: id -> порядковый номер в groups
    SalesReport report;
    std::map<std::int64_t, std::size_t> group_index;
    auto const group_of = [&](std::int64_t id, std::string const &fallback) {
        auto const it = group_index.find(id);
        if (it != group_index.end()) {
            return it->second;
        }
        SalesGroup g;
        g.group_id = id;
        g.name = id == 0 ? fallback : group_names.count(id) ? group_names.at(id) : "#" + std::to_string(id);
        group_index.emplace(id, report.groups.size());
        report.groups.push_back(std::move(g));
        return report.groups.size() - 1;
    };

    while (items.step()) {
        SalesLine line;
        line.item_id = items.column_int(0);
        line.name = items.column_text(1);
        line.unit = unit_from_string(items.column_text(2));
        auto const gid = items.column_int(3);
        line.group_id = gid;
        line.group_name = gid == 0 ? "Без группы" : (group_names.count(gid) ? group_names.at(gid) : "#" + std::to_string(gid));
        line.qty = items.column_int(4);
        line.cost = to_kop(items.column_int(5));
        line.revenue = to_kop(items.column_int(6));
        line.margin = line.revenue - line.cost;
        line.margin_pct = pct(line.margin, line.cost);
        report.lines_without_cost += items.column_int(7);
        report.lines_without_price += items.column_int(8);
        if (line.qty != 0 || line.cost != 0 || line.revenue != 0) {
            auto const idx = group_of(gid, line.group_name);
            auto &g = report.groups[idx];
            g.items.push_back(line);
            g.qty += line.qty;
            g.cost += line.cost;
            g.revenue += line.revenue;
        }
    }

    for (auto &g : report.groups) {
        g.margin = g.revenue - g.cost;
        g.margin_pct = pct(g.margin, g.cost);
        report.total_qty += g.qty;
        report.total_cost += g.cost;
        report.total_revenue += g.revenue;
    }
    report.total_margin = report.total_revenue - report.total_cost;
    report.total_margin_pct = pct(report.total_margin, report.total_cost);

    auto const by_name = [](SalesGroup const &a, SalesGroup const &b) { return a.name < b.name; };
    std::sort(report.groups.begin(), report.groups.end(), by_name);
    report.groups.erase(std::remove_if(report.groups.begin(), report.groups.end(), [](SalesGroup const &g) { return g.items.empty(); }),
                        report.groups.end());
    return report;
}

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

    // переоценки остатка при установке себестоимости и цен выбранного типа
    Revaluation reval;
    add_revaluations(db, COST_PRICE_TYPE, begin, end, group, false, reval);
    if (price_type) {
        add_revaluations(db, *price_type, begin, end, group, true, reval);
    }

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
        // произведение количества (тысячные) на цену (копейки) делится на 1000 с округлением;
        // стоимость остатка включает переоценки, иначе не равна количеству × себестоимости
        auto const item_reval = reval.by_item.find(r.item_id);
        auto const [reval_before, reval_total] = item_reval == reval.by_item.end() ? std::pair<std::int64_t, std::int64_t>{} : item_reval->second;
        r.opening_sum = to_kop(items.column_int(6) + reval_before);
        r.closing_sum = to_kop(items.column_int(7) + reval_total);
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
        report.opening_price_sum = to_kop(value + reval.before_price);
        report.moves_without_price = without;
    }

    // документы периода с суммами по себестоимости и по типу цен: tp - цена типа на момент движения,
    // lp - цена в строке документа; реализация по типу цен - по цене продажи, разница - переоценка
    std::string const line = price_type ? LINE_PRICE : "NULL";
    // x материализуется: иначе SQLite подставит подзапрос и будет искать цену заново при каждом упоминании tp
    auto docs = db.prepare(
        "WITH x AS MATERIALIZED (SELECT m.doc_id, m.qty, m.cost_kop, " + price + " AS tp, " + line +
        " AS lp FROM stock_moves m JOIN items i ON i.id = m.item_id WHERE m.date >= ?1 AND m.date <= ?2 AND " + std::string(IN_GROUP) +
        ") SELECT d.id, d.type, d.number, d.date, coalesce(p.name, ''), sum(x.qty * coalesce(x.cost_kop, 0)), x.qty > 0, "
        "coalesce(sum(x.qty * coalesce(CASE WHEN " +
        std::string(SOLD_AT_LINE) + " THEN x.lp ELSE x.tp END, 0)), 0), coalesce(sum(x.tp IS NULL), 0), coalesce(sum(CASE WHEN " + SOLD_AT_LINE +
        " AND x.lp > x.tp THEN -x.qty * (x.lp - x.tp) END), 0), coalesce(sum(CASE WHEN " + SOLD_AT_LINE +
        " AND x.lp < x.tp THEN x.qty * (x.lp - x.tp) END), 0)"
        " FROM x JOIN docs d ON d.id = x.doc_id LEFT JOIN partners p ON p.id = d.partner_id GROUP BY d.id, x.qty > 0 ORDER BY d.date, d.id");
    docs.bind(1, begin).bind(2, end);
    bind_group(docs, group);
    if (price_type) {
        docs.bind(4, *price_type);
    }
    std::vector<ReportDoc> sale_revaluations;
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
        d.sum = to_kop(d.incoming ? value : -value);
        auto const price_value = docs.column_int(7);
        d.price_sum = to_kop(d.incoming ? price_value : -price_value);
        if (price_type) {
            report.moves_without_price += docs.column_int(8);
        }
        // продано дороже цены типа - переоценка в приход, дешевле - в расход; отдельными строками
        for (auto const [column, incoming] : {std::pair{9, true}, std::pair{10, false}}) {
            if (auto const diff = docs.column_int(column); diff != 0) {
                ReportDoc r = d;
                r.revaluation = true;
                r.incoming = incoming;
                r.sum = 0;
                r.price_sum = to_kop(diff);
                sale_revaluations.push_back(std::move(r));
            }
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
    for (auto &r : sale_revaluations) {
        if (r.incoming) {
            report.in_price_sum += r.price_sum;
            report.incoming.push_back(std::move(r));
        } else {
            report.out_price_sum += r.price_sum;
            report.outgoing.push_back(std::move(r));
        }
    }
    // остаток на конец по арифметике отчёта, чтобы начало + приход - расход сходилось до копейки
    auto head = db.prepare("SELECT number, date FROM docs WHERE id = ?");
    for (auto const &[id, amounts] : reval.by_doc) {
        auto const [cost, price] = amounts;
        if (cost == 0 && price == 0) {
            continue;
        }
        ReportDoc d;
        d.id = id;
        d.type = DocType::PRICES;
        head.reset();
        head.bind(1, id);
        head.step();
        d.number = head.column_int(0);
        auto const when = head.column_text(1);
        d.date = when.substr(0, 10);
        d.time = when.substr(11);
        // рост цены - приход, снижение - расход; знак по колонке, где переоценка есть
        d.incoming = (cost != 0 ? cost : price) > 0;
        auto const sign = d.incoming ? 1 : -1;
        d.sum = to_kop(sign * cost);
        d.price_sum = to_kop(sign * price);
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
    auto const chronological = [](ReportDoc const &a, ReportDoc const &b) {
        return std::tie(a.date, a.time, a.id, a.revaluation) < std::tie(b.date, b.time, b.id, b.revaluation);
    };
    std::sort(report.incoming.begin(), report.incoming.end(), chronological);
    std::sort(report.outgoing.begin(), report.outgoing.end(), chronological);

    report.closing_sum = report.opening_sum + report.in_sum - report.out_sum;
    report.closing_price_sum = report.opening_price_sum + report.in_price_sum - report.out_price_sum;
    return report;
}
