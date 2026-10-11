#include "report.hpp"

#include "errors.hpp"

namespace {

/// Позиции группы ?3 и её подгрупп (или все, если ?3 пусто).
constexpr char const IN_GROUP[] =
    "(?3 IS NULL OR i.group_id IN (WITH RECURSIVE sub(id) AS (SELECT ?3 UNION SELECT g.id FROM groups g "
    "JOIN sub ON g.parent_id = sub.id) SELECT id FROM sub))";

void bind_group(Statement &stmt, std::optional<std::int64_t> group) {
    if (group) {
        stmt.bind(3, *group);
    }
}

} // namespace

GoodsReport goods_report(Database &db, std::string const &from, std::string const &to, std::optional<std::int64_t> group) {
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
                                        " coalesce(sum(CASE WHEN m.date < ?1 THEN m.qty * coalesce(l.cost_kop, 0) END), 0),"
                                        " coalesce(sum(m.qty * coalesce(l.cost_kop, 0)), 0),"
                                        " coalesce(sum(CASE WHEN l.cost_kop IS NULL THEN 1 END), 0)"
                                        " FROM stock_moves m JOIN items i ON i.id = m.item_id"
                                        " JOIN doc_lines l ON l.doc_id = m.doc_id AND l.item_id = m.item_id"
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

    // документы периода с суммой по себестоимости
    auto docs = db.prepare(std::string("SELECT d.id, d.type, d.number, d.date, coalesce(p.name, ''), sum(m.qty * coalesce(l.cost_kop, 0))"
                                       " FROM stock_moves m JOIN docs d ON d.id = m.doc_id JOIN items i ON i.id = m.item_id"
                                       " JOIN doc_lines l ON l.doc_id = m.doc_id AND l.item_id = m.item_id"
                                       " LEFT JOIN partners p ON p.id = d.partner_id"
                                       " WHERE m.date >= ?1 AND m.date <= ?2 AND ") +
                           IN_GROUP + " GROUP BY d.id ORDER BY d.date, d.id");
    docs.bind(1, begin).bind(2, end);
    bind_group(docs, group);
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
        d.incoming = value >= 0 && (d.type == DocType::PRODUCTION || d.type == DocType::RETURN);
        d.sum = ((d.incoming ? value : -value) + 500) / 1000;
        if (d.incoming) {
            report.in_sum += d.sum;
            report.incoming.push_back(std::move(d));
        } else {
            report.out_sum += d.sum;
            report.outgoing.push_back(std::move(d));
        }
    }
    // остаток на конец по арифметике отчёта, чтобы начало + приход - расход сходилось до копейки
    report.closing_sum = report.opening_sum + report.in_sum - report.out_sum;
    return report;
}
