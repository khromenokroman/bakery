#include "repost.hpp"

#include <tuple>

#include "errors.hpp"
#include "period.hpp"

namespace {

/// Проведённые складские документы после места (?1, ?2) в порядке проведения.
constexpr char const AFTER[] =
    " FROM docs WHERE status = 'posted' AND type IN ('production', 'sale', 'return', 'writeoff', 'inventory')"
    " AND (date > ?1 OR (date = ?1 AND id > ?2))";

} // namespace

Reposter::Reposter(Database &db) : m_db{db} {}

RepostState Reposter::state() const {
    auto stmt = m_db.prepare("SELECT from_date, from_id, cause_id, epoch FROM repost WHERE id = 1");
    RepostState out;
    if (!stmt.step()) {
        return out;
    }
    if (!stmt.column_is_null(0)) {
        out.from = DocKey{stmt.column_text(0), stmt.column_int(1)};
    }
    if (!stmt.column_is_null(2)) {
        out.cause_id = stmt.column_int(2);
    }
    out.epoch = stmt.column_int(3);
    return out;
}

void Reposter::mark(DocKey const &key, std::int64_t cause_id) {
    auto const s = state();
    auto const earlier = !s.from || std::tie(key.moment, key.id) < std::tie(s.from->moment, s.from->id);
    if (earlier) {
        auto stmt = m_db.prepare("UPDATE repost SET from_date = ?, from_id = ?, cause_id = ?, epoch = epoch + 1 WHERE id = 1");
        stmt.bind(1, key.moment).bind(2, key.id).bind(3, cause_id);
        stmt.step();
    } else {
        m_db.exec("UPDATE repost SET epoch = epoch + 1 WHERE id = 1");
    }
}

std::int64_t Reposter::count(std::string const &from) const {
    Period{m_db}.check_open(from, "перепроводить документы с " + ru_moment(from));
    auto stmt = m_db.prepare(std::string("SELECT count(*)") + AFTER);
    stmt.bind(1, from).bind(2, 0);
    stmt.step();
    return stmt.column_int(0);
}

RepostStep Reposter::step(std::string const &from, std::optional<DocKey> const &after, std::int64_t epoch) {
    Period{m_db}.check_open(from, "перепроводить документы с " + ru_moment(from));
    auto const cursor = after.value_or(DocKey{from, 0});
    auto next = m_db.prepare(std::string("SELECT id") + AFTER + " ORDER BY date, id LIMIT 1");
    next.bind(1, cursor.moment).bind(2, cursor.id);
    RepostStep out;
    if (!next.step()) {
        // дошли до конца: отметку снимает только перепроведение, начатое не позже неё и без новых отметок
        auto const s = state();
        if (s.from && s.epoch == epoch && from <= s.from->moment) {
            m_db.exec("UPDATE repost SET from_date = NULL, from_id = NULL, cause_id = NULL WHERE id = 1");
        }
        out.needed = state().from.has_value();
        return out;
    }
    Documents docs{m_db};
    auto const id = next.column_int(0);
    try {
        docs.repost(id);
    } catch (UserError const &e) {
        out.error = e.what();
    }
    out.doc = docs.get(id);
    auto left = m_db.prepare(std::string("SELECT count(*)") + AFTER);
    left.bind(1, moment(*out.doc)).bind(2, id);
    left.step();
    out.remaining = left.column_int(0);
    out.needed = state().from.has_value();
    return out;
}
