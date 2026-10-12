#include "period.hpp"

#include "docs.hpp"
#include "errors.hpp"
#include "repost.hpp"

namespace {

constexpr char const *CLOSED_KEY = "closed_till";

/// "30.09.2026" из "2026-09-30".
std::string ru_date(std::string const &iso) { return ru_moment(iso + " 00:00:00"); }

} // namespace

Period::Period(Database &db) : m_db{db} {}

std::optional<std::string> Period::closed_till() const {
    auto stmt = m_db.prepare("SELECT value FROM settings WHERE key = ?");
    stmt.bind(1, CLOSED_KEY);
    if (!stmt.step() || stmt.column_text(0).empty()) {
        return std::nullopt;
    }
    return stmt.column_text(0);
}

void Period::close(std::optional<std::string> const &till) {
    Transaction tx{m_db};
    if (till) {
        check_date(*till);
        auto const old = closed_till();
        auto const state = Reposter{m_db}.state();
        if ((!old || *till > *old) && state.from && state.from->moment.substr(0, 10) <= *till) {
            throw UserError("нельзя закрыть период по " + ru_date(*till) + ": документы с " + ru_moment(state.from->moment) +
                            " ждут перепроведения, сначала перепроведите их");
        }
        auto stmt = m_db.prepare("INSERT INTO settings (key, value) VALUES (?, ?) ON CONFLICT (key) DO UPDATE SET value = excluded.value");
        stmt.bind(1, CLOSED_KEY).bind(2, *till);
        stmt.step();
    } else {
        auto stmt = m_db.prepare("DELETE FROM settings WHERE key = ?");
        stmt.bind(1, CLOSED_KEY);
        stmt.step();
    }
    tx.commit();
}

bool Period::is_closed(std::string const &moment) const {
    auto const till = closed_till();
    return till && moment.substr(0, 10) <= *till;
}

void Period::check_open(std::string const &moment, std::string const &what) const {
    if (is_closed(moment)) {
        throw UserError("период по " + ru_date(*closed_till()) + " закрыт: " + what + " нельзя");
    }
}
