#include "partners.hpp"

#include "errors.hpp"
#include "settings.hpp"

namespace {

constexpr std::size_t MAX_NAME = 200;
constexpr std::size_t MAX_TEXT = 1000;

std::string trim(std::string const &text) {
    auto const first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return {};
    }
    return text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
}

Partner read(Statement const &stmt) {
    Partner p;
    p.id = stmt.column_int(0);
    p.name = stmt.column_text(1);
    p.phone = stmt.column_text(2);
    p.note = stmt.column_text(3);
    p.archived = stmt.column_int(4) != 0;
    p.inn = stmt.column_text(5);
    p.kpp = stmt.column_text(6);
    p.address = stmt.column_text(7);
    return p;
}

} // namespace

Partners::Partners(Database &db) : m_db{db} {}

std::vector<Partner> Partners::list(bool with_archived) const {
    auto stmt = m_db.prepare("SELECT id, name, phone, note, archived, inn, kpp, address FROM partners WHERE ? OR archived = 0 ORDER BY name");
    stmt.bind(1, static_cast<std::int64_t>(with_archived));
    std::vector<Partner> out;
    while (stmt.step()) {
        out.push_back(read(stmt));
    }
    return out;
}

Partner Partners::get(std::int64_t id) const {
    auto stmt = m_db.prepare("SELECT id, name, phone, note, archived, inn, kpp, address FROM partners WHERE id = ?");
    stmt.bind(1, id);
    if (!stmt.step()) {
        throw NotFoundError("нет клиента с id " + std::to_string(id));
    }
    return read(stmt);
}

std::int64_t Partners::create(Partner const &partner) {
    Transaction tx{m_db};
    auto const p = clean(partner);
    auto stmt = m_db.prepare("INSERT INTO partners (name, phone, note, archived, inn, kpp, address) VALUES (?, ?, ?, ?, ?, ?, ?)");
    stmt.bind(1, p.name)
        .bind(2, p.phone)
        .bind(3, p.note)
        .bind(4, static_cast<std::int64_t>(p.archived))
        .bind(5, p.inn)
        .bind(6, p.kpp)
        .bind(7, p.address);
    stmt.step();
    auto const id = m_db.last_insert_id();
    tx.commit();
    return id;
}

void Partners::update(Partner const &partner) {
    Transaction tx{m_db};
    (void)get(partner.id);
    auto const p = clean(partner);
    auto stmt = m_db.prepare("UPDATE partners SET name = ?, phone = ?, note = ?, archived = ?, inn = ?, kpp = ?, address = ? WHERE id = ?");
    stmt.bind(1, p.name)
        .bind(2, p.phone)
        .bind(3, p.note)
        .bind(4, static_cast<std::int64_t>(p.archived))
        .bind(5, p.inn)
        .bind(6, p.kpp)
        .bind(7, p.address)
        .bind(8, p.id);
    stmt.step();
    tx.commit();
}

void Partners::remove(std::int64_t id) {
    Transaction tx{m_db};
    auto const p = get(id);
    auto used = m_db.prepare("SELECT count(*) FROM docs WHERE partner_id = ?");
    used.bind(1, id);
    used.step();
    if (auto const n = used.column_int(0); n != 0) {
        throw UserError("«" + p.name + "» есть в документах (" + std::to_string(n) + "), удалить нельзя: отправьте в архив");
    }
    auto del = m_db.prepare("DELETE FROM partners WHERE id = ?");
    del.bind(1, id);
    del.step();
    tx.commit();
}

Partner Partners::clean(Partner const &partner) const {
    auto p = partner;
    p.name = trim(p.name);
    p.phone = trim(p.phone);
    p.note = trim(p.note);
    p.inn = trim(p.inn);
    p.kpp = trim(p.kpp);
    p.address = trim(p.address);
    check_inn(p.inn);
    check_kpp(p.kpp);
    if (p.address.size() > MAX_TEXT) {
        throw UserError("слишком длинный адрес клиента");
    }
    if (p.name.empty()) {
        throw UserError("название клиента не может быть пустым");
    }
    if (p.name.size() > MAX_NAME || p.phone.size() > MAX_NAME || p.note.size() > MAX_TEXT) {
        throw UserError("слишком длинное название, телефон или примечание клиента");
    }
    auto dup = m_db.prepare("SELECT count(*) FROM partners WHERE name = ? AND id <> ?");
    dup.bind(1, p.name).bind(2, p.id);
    dup.step();
    if (dup.column_int(0) != 0) {
        throw UserError("клиент «" + p.name + "» уже есть");
    }
    return p;
}
