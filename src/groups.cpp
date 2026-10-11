#include "groups.hpp"

#include "errors.hpp"

namespace {

constexpr std::size_t MAX_NAME = 200;

std::string trim(std::string const &text) {
    auto const first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return {};
    }
    return text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
}

void bind_parent(Statement &stmt, int index, std::optional<std::int64_t> parent) {
    if (parent) {
        stmt.bind(index, *parent);
    } else {
        stmt.bind_null(index);
    }
}

} // namespace

Groups::Groups(Database &db) : m_db{db} {}

std::vector<Group> Groups::list() const {
    auto stmt = m_db.prepare("SELECT id, name, parent_id FROM groups ORDER BY name");
    std::vector<Group> out;
    while (stmt.step()) {
        Group g;
        g.id = stmt.column_int(0);
        g.name = stmt.column_text(1);
        if (!stmt.column_is_null(2)) {
            g.parent_id = stmt.column_int(2);
        }
        out.push_back(std::move(g));
    }
    return out;
}

Group Groups::get(std::int64_t id) const {
    auto stmt = m_db.prepare("SELECT name, parent_id FROM groups WHERE id = ?");
    stmt.bind(1, id);
    if (!stmt.step()) {
        throw NotFoundError("нет группы с id " + std::to_string(id));
    }
    Group g;
    g.id = id;
    g.name = stmt.column_text(0);
    if (!stmt.column_is_null(1)) {
        g.parent_id = stmt.column_int(1);
    }
    return g;
}

std::int64_t Groups::create(Group const &group) {
    Transaction tx{m_db};
    auto g = group;
    g.name = trim(g.name);
    g.id = 0;
    check_name(g);
    auto stmt = m_db.prepare("INSERT INTO groups (name, parent_id) VALUES (?, ?)");
    stmt.bind(1, g.name);
    bind_parent(stmt, 2, g.parent_id);
    stmt.step();
    auto const id = m_db.last_insert_id();
    tx.commit();
    return id;
}

void Groups::update(Group const &group) {
    Transaction tx{m_db};
    (void)get(group.id);
    auto g = group;
    g.name = trim(g.name);
    if (g.parent_id && with_descendants(group.id).contains(*g.parent_id)) {
        throw UserError("группу «" + g.name + "» нельзя вложить в саму себя или в её подгруппу");
    }
    check_name(g);
    auto stmt = m_db.prepare("UPDATE groups SET name = ?, parent_id = ? WHERE id = ?");
    stmt.bind(1, g.name);
    bind_parent(stmt, 2, g.parent_id);
    stmt.bind(3, g.id);
    stmt.step();
    tx.commit();
}

void Groups::remove(std::int64_t id) {
    Transaction tx{m_db};
    auto const g = get(id);
    auto children = m_db.prepare("SELECT count(*) FROM groups WHERE parent_id = ?");
    children.bind(1, id);
    children.step();
    if (children.column_int(0) != 0) {
        throw UserError("в группе «" + g.name + "» есть подгруппы, сначала удалите или перенесите их");
    }
    auto items = m_db.prepare("SELECT count(*) FROM items WHERE group_id = ?");
    items.bind(1, id);
    items.step();
    if (auto const n = items.column_int(0); n != 0) {
        throw UserError("в группе «" + g.name + "» есть позиции (" + std::to_string(n) + "), сначала перенесите их");
    }
    auto del = m_db.prepare("DELETE FROM groups WHERE id = ?");
    del.bind(1, id);
    del.step();
    tx.commit();
}

std::set<std::int64_t> Groups::with_descendants(std::int64_t id) const {
    auto stmt =
        m_db.prepare("WITH RECURSIVE sub(id) AS (SELECT ? UNION SELECT g.id FROM groups g JOIN sub ON g.parent_id = sub.id) SELECT id FROM sub");
    stmt.bind(1, id);
    std::set<std::int64_t> out;
    while (stmt.step()) {
        out.insert(stmt.column_int(0));
    }
    return out;
}

std::string Groups::path(std::int64_t id) const {
    std::string out;
    std::optional<std::int64_t> cur = id;
    for (int depth = 0; cur && depth < 100; ++depth) {
        auto const g = get(*cur);
        out = out.empty() ? g.name : g.name + " / " + out;
        cur = g.parent_id;
    }
    return out;
}

void Groups::check_name(Group const &group) const {
    if (group.name.empty()) {
        throw UserError("название группы не может быть пустым");
    }
    if (group.name.size() > MAX_NAME) {
        throw UserError("название группы длиннее " + std::to_string(MAX_NAME) + " байт");
    }
    if (group.parent_id) {
        (void)get(*group.parent_id);
    }
    auto stmt = m_db.prepare("SELECT count(*) FROM groups WHERE name = ? AND parent_id IS ? AND id <> ?");
    stmt.bind(1, group.name);
    bind_parent(stmt, 2, group.parent_id);
    stmt.bind(3, group.id);
    stmt.step();
    if (stmt.column_int(0) != 0) {
        throw UserError("группа «" + group.name + "» здесь уже есть");
    }
}
