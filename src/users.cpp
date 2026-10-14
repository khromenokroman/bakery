#include "users.hpp"

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>

#include <array>
#include <cstdio>
#include <stdexcept>

#include "errors.hpp"

namespace {

constexpr int PBKDF2_ITERATIONS = 200000;
constexpr std::size_t SALT_BYTES = 16;
constexpr std::size_t HASH_BYTES = 32;
constexpr std::size_t TOKEN_BYTES = 32;
constexpr std::size_t MIN_PASSWORD = 6;
constexpr std::size_t MAX_LOGIN = 50;

std::string to_hex(unsigned char const *data, std::size_t size) {
    static constexpr char DIGITS[] = "0123456789abcdef";
    std::string out;
    out.reserve(size * 2);
    for (std::size_t i = 0; i < size; ++i) {
        out += DIGITS[data[i] >> 4];
        out += DIGITS[data[i] & 0xF];
    }
    return out;
}

std::string random_hex(std::size_t bytes) {
    std::array<unsigned char, 64> buf{};
    if (bytes > buf.size() || RAND_bytes(buf.data(), static_cast<int>(bytes)) != 1) {
        throw std::runtime_error("не удалось получить случайные байты");
    }
    return to_hex(buf.data(), bytes);
}

/// PBKDF2-HMAC-SHA256 пароля с солью в шестнадцатеричном виде.
std::string pbkdf2(std::string_view password, std::string_view salt, int iterations) {
    std::array<unsigned char, HASH_BYTES> out{};
    if (PKCS5_PBKDF2_HMAC(password.data(), static_cast<int>(password.size()), reinterpret_cast<unsigned char const *>(salt.data()),
                          static_cast<int>(salt.size()), iterations, EVP_sha256(), static_cast<int>(out.size()), out.data()) != 1) {
        throw std::runtime_error("ошибка PBKDF2");
    }
    return to_hex(out.data(), out.size());
}

/// Хеш пароля для хранения: "pbkdf2-sha256$итерации$соль$хеш".
std::string hash_password(std::string_view password) {
    auto const salt = random_hex(SALT_BYTES);
    return "pbkdf2-sha256$" + std::to_string(PBKDF2_ITERATIONS) + "$" + salt + "$" + pbkdf2(password, salt, PBKDF2_ITERATIONS);
}

bool verify_password(std::string_view password, std::string const &stored) {
    auto const a = stored.find('$');
    auto const b = stored.find('$', a + 1);
    auto const c = stored.find('$', b + 1);
    if (a == std::string::npos || b == std::string::npos || c == std::string::npos || stored.substr(0, a) != "pbkdf2-sha256") {
        return false;
    }
    auto const iterations = std::stoi(stored.substr(a + 1, b - a - 1));
    auto const expected = stored.substr(c + 1);
    auto const actual = pbkdf2(password, stored.substr(b + 1, c - b - 1), iterations);
    return actual.size() == expected.size() && CRYPTO_memcmp(actual.data(), expected.data(), actual.size()) == 0;
}

std::string sha256_hex(std::string_view text) {
    std::array<unsigned char, HASH_BYTES> out{};
    unsigned int size = 0;
    if (EVP_Digest(text.data(), text.size(), out.data(), &size, EVP_sha256(), nullptr) != 1) {
        throw std::runtime_error("ошибка SHA-256");
    }
    return to_hex(out.data(), size);
}

void check_password_length(std::string_view password) {
    if (password.size() < MIN_PASSWORD) {
        throw UserError("пароль короче " + std::to_string(MIN_PASSWORD) + " символов");
    }
}

std::string trim(std::string const &text) {
    auto const first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return {};
    }
    return text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
}

User read_user(Statement const &stmt) {
    User user;
    user.id = stmt.column_int(0);
    user.login = stmt.column_text(1);
    user.name = stmt.column_text(2);
    user.role = role_from_string(stmt.column_text(3));
    user.blocked = stmt.column_int(4) != 0;
    return user;
}

constexpr char const SELECT_USER[] = "SELECT id, login, name, role, blocked FROM users ";

/// Действующих администраторов, кроме пользователя except.
std::int64_t active_admins(Database &db, std::int64_t except) {
    auto stmt = db.prepare("SELECT count(*) FROM users WHERE role = 'admin' AND blocked = 0 AND id <> ?");
    stmt.bind(1, except);
    stmt.step();
    return stmt.column_int(0);
}

void close_sessions(Database &db, std::int64_t user_id) {
    auto stmt = db.prepare("DELETE FROM sessions WHERE user_id = ?");
    stmt.bind(1, user_id);
    stmt.step();
}

} // namespace

std::string_view to_string(Role role) {
    switch (role) {
        case Role::ADMIN:
            return "admin";
        case Role::TECHNOLOGIST:
            return "technologist";
        case Role::SELLER:
            return "seller";
    }
    return "seller";
}

Role role_from_string(std::string_view text) {
    if (text == "admin") {
        return Role::ADMIN;
    }
    if (text == "technologist") {
        return Role::TECHNOLOGIST;
    }
    if (text == "seller") {
        return Role::SELLER;
    }
    throw UserError("неизвестная роль: " + std::string(text) + " (admin, technologist, seller)");
}

Users::Users(Database &db) : m_db{db} {}

bool Users::empty() const {
    auto stmt = m_db.prepare("SELECT count(*) FROM users");
    stmt.step();
    return stmt.column_int(0) == 0;
}

std::vector<User> Users::list() const {
    auto stmt = m_db.prepare(std::string(SELECT_USER) + "ORDER BY login");
    std::vector<User> out;
    while (stmt.step()) {
        out.push_back(read_user(stmt));
    }
    return out;
}

User Users::get(std::int64_t id) const {
    auto stmt = m_db.prepare(std::string(SELECT_USER) + "WHERE id = ?");
    stmt.bind(1, id);
    if (!stmt.step()) {
        throw NotFoundError("нет пользователя с id " + std::to_string(id));
    }
    return read_user(stmt);
}

std::int64_t Users::setup(User const &user, std::string_view password) {
    Transaction tx{m_db};
    if (!empty()) {
        throw UserError("первый пользователь уже создан: войдите под ним");
    }
    auto admin = user;
    admin.role = Role::ADMIN;
    admin.blocked = false;
    auto const id = create(admin, password);
    tx.commit();
    return id;
}

std::int64_t Users::create(User const &input, std::string_view password) {
    auto user = input;
    validate(user, 0);
    check_password_length(password);
    auto stmt = m_db.prepare("INSERT INTO users (login, name, pass_hash, role, blocked) VALUES (?, ?, ?, ?, ?)");
    stmt.bind(1, user.login).bind(2, user.name).bind(3, hash_password(password)).bind(4, to_string(user.role)).bind(5, user.blocked ? 1 : 0);
    stmt.step();
    return m_db.last_insert_id();
}

void Users::update(User const &input) {
    Transaction tx{m_db};
    auto const old = get(input.id);
    auto user = input;
    user.login = old.login;
    validate(user, user.id);
    if ((user.role != Role::ADMIN || user.blocked) && old.role == Role::ADMIN && !old.blocked && active_admins(m_db, user.id) == 0) {
        throw UserError("«" + old.login + "» - последний администратор: должен остаться хотя бы один");
    }
    auto stmt = m_db.prepare("UPDATE users SET name = ?, role = ?, blocked = ? WHERE id = ?");
    stmt.bind(1, user.name).bind(2, to_string(user.role)).bind(3, user.blocked ? 1 : 0).bind(4, user.id);
    stmt.step();
    if (user.blocked) {
        close_sessions(m_db, user.id);
    }
    tx.commit();
}

void Users::set_password(std::int64_t id, std::string_view password) {
    (void)get(id);
    check_password_length(password);
    Transaction tx{m_db};
    auto stmt = m_db.prepare("UPDATE users SET pass_hash = ? WHERE id = ?");
    stmt.bind(1, hash_password(password)).bind(2, id);
    stmt.step();
    close_sessions(m_db, id);
    tx.commit();
}

bool Users::check_password(std::int64_t id, std::string_view password) const {
    auto stmt = m_db.prepare("SELECT pass_hash FROM users WHERE id = ?");
    stmt.bind(1, id);
    return stmt.step() && verify_password(password, stmt.column_text(0));
}

std::string Users::login(std::string_view login, std::string_view password) {
    auto stmt = m_db.prepare("SELECT id, pass_hash, blocked FROM users WHERE login = ? COLLATE NOCASE");
    stmt.bind(1, trim(std::string(login)));
    // на неизвестный логин тоже считается хеш: время ответа не выдаёт, есть ли такой пользователь
    auto const found = stmt.step();
    auto const ok = verify_password(password, found ? stmt.column_text(1) : hash_password("-"));
    if (!found || !ok) {
        throw UserError("неверный логин или пароль");
    }
    if (stmt.column_int(2) != 0) {
        throw UserError("пользователь заблокирован");
    }
    return open_session(stmt.column_int(0));
}

std::string Users::open_session(std::int64_t id) {
    auto const token = random_hex(TOKEN_BYTES);
    auto clean = m_db.prepare("DELETE FROM sessions WHERE expires < datetime('now')");
    clean.step();
    auto stmt = m_db.prepare("INSERT INTO sessions (token_hash, user_id, expires) VALUES (?, ?, datetime('now', ?))");
    stmt.bind(1, sha256_hex(token)).bind(2, id).bind(3, "+" + std::to_string(SESSION_DAYS) + " days");
    stmt.step();
    return token;
}

std::optional<User> Users::by_session(std::string_view token) const {
    if (token.empty()) {
        return std::nullopt;
    }
    auto stmt = m_db.prepare(
        "SELECT u.id, u.login, u.name, u.role, u.blocked FROM sessions s JOIN users u ON u.id = s.user_id "
        "WHERE s.token_hash = ? AND s.expires >= datetime('now') AND u.blocked = 0");
    stmt.bind(1, sha256_hex(token));
    if (!stmt.step()) {
        return std::nullopt;
    }
    return read_user(stmt);
}

void Users::logout(std::string_view token) {
    auto stmt = m_db.prepare("DELETE FROM sessions WHERE token_hash = ?");
    stmt.bind(1, sha256_hex(token));
    stmt.step();
}

void Users::validate(User &user, std::int64_t except) const {
    user.login = trim(user.login);
    user.name = trim(user.name);
    if (user.login.empty()) {
        throw UserError("логин не может быть пустым");
    }
    if (user.login.size() > MAX_LOGIN) {
        throw UserError("логин длиннее " + std::to_string(MAX_LOGIN) + " байт");
    }
    if (user.login.find_first_of(" \t") != std::string::npos) {
        throw UserError("в логине «" + user.login + "» не должно быть пробелов");
    }
    if (user.name.size() > 200) {
        throw UserError("имя длиннее 200 байт");
    }
    auto dup = m_db.prepare("SELECT count(*) FROM users WHERE login = ? COLLATE NOCASE AND id <> ?");
    dup.bind(1, user.login).bind(2, except);
    dup.step();
    if (dup.column_int(0) != 0) {
        throw UserError("логин «" + user.login + "» уже занят");
    }
}
