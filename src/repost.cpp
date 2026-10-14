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

std::int64_t Reposter::remaining(DocKey const &after) const {
    auto stmt = m_db.prepare(std::string("SELECT count(*)") + AFTER);
    stmt.bind(1, after.moment).bind(2, after.id);
    stmt.step();
    return stmt.column_int(0);
}

std::optional<RepostRun> Reposter::run() const {
    auto stmt = m_db.prepare(
        "SELECT run_from, run_after_date, run_after_id, run_epoch, run_done, coalesce(run_updated, ''), run_error_id, "
        "coalesce(run_error, '') FROM repost WHERE id = 1 AND run_from IS NOT NULL");
    if (!stmt.step()) {
        return std::nullopt;
    }
    RepostRun r;
    r.from = stmt.column_text(0);
    if (!stmt.column_is_null(1)) {
        r.after = DocKey{stmt.column_text(1), stmt.column_int(2)};
    }
    r.epoch = stmt.column_int(3);
    r.done = stmt.column_int(4);
    r.updated = stmt.column_text(5);
    if (!stmt.column_is_null(6)) {
        r.error_id = stmt.column_int(6);
    }
    r.error = stmt.column_text(7);
    return r;
}

void Reposter::reset() {
    m_db.exec(
        "UPDATE repost SET run_from = NULL, run_after_date = NULL, run_after_id = NULL, run_epoch = 0, run_done = 0, run_updated = NULL, "
        "run_error_id = NULL, run_error = NULL WHERE id = 1");
}

RepostStep Reposter::step(std::string const &from, bool restart) {
    Period{m_db}.check_open(from, "перепроводить документы с " + ru_moment(from));
    auto current = run();
    if (!current || current->from != from || restart) {
        // новый прогон: номер отметок на начало - если за время работы появится новая правка задним
        // числом, отметка не снимется
        current = RepostRun{.from = from, .epoch = state().epoch};
        auto start = m_db.prepare(
            "UPDATE repost SET run_from = ?, run_after_date = NULL, run_after_id = NULL, run_epoch = ?, run_done = 0, "
            "run_updated = datetime('now', 'localtime'), run_error_id = NULL, run_error = NULL WHERE id = 1");
        start.bind(1, from).bind(2, current->epoch);
        start.step();
    }
    auto const cursor = current->after.value_or(DocKey{from, 0});
    auto next = m_db.prepare(std::string("SELECT id") + AFTER + " ORDER BY date, id LIMIT 1");
    next.bind(1, cursor.moment).bind(2, cursor.id);
    RepostStep out;
    out.done = current->done;
    if (!next.step()) {
        // дошли до конца: отметку снимает только перепроведение, начатое не позже неё и без новых отметок
        auto const s = state();
        if (s.from && s.epoch == current->epoch && from <= s.from->moment) {
            m_db.exec("UPDATE repost SET from_date = NULL, from_id = NULL, cause_id = NULL WHERE id = 1");
        }
        reset();
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
    if (out.error) {
        auto save = m_db.prepare("UPDATE repost SET run_error_id = ?, run_error = ?, run_updated = datetime('now', 'localtime') WHERE id = 1");
        save.bind(1, id).bind(2, *out.error);
        save.step();
    } else {
        out.done += 1;
        auto save = m_db.prepare(
            "UPDATE repost SET run_after_date = ?, run_after_id = ?, run_done = ?, run_error_id = NULL, run_error = NULL, "
            "run_updated = datetime('now', 'localtime') WHERE id = 1");
        save.bind(1, moment(*out.doc)).bind(2, id).bind(3, out.done);
        save.step();
    }
    out.remaining = remaining(DocKey{moment(*out.doc), id});
    out.needed = state().from.has_value();
    return out;
}
