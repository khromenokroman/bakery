#include "db.hpp"

#include <sqlite3.h>

namespace {

[[noreturn]] void fail(sqlite3 *db, std::string_view what) {
    throw DbError(std::string(what) + ": " + (db != nullptr ? sqlite3_errmsg(db) : "нет соединения"));
}

} // namespace

void Statement::Finalizer::operator()(sqlite3_stmt *stmt) const noexcept { sqlite3_finalize(stmt); }

Statement::Statement(sqlite3 *db, std::string_view sql) : m_db{db} {
    sqlite3_stmt *stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql.data(), static_cast<int>(sql.size()), &stmt, nullptr) != SQLITE_OK) {
        fail(db, "ошибка подготовки запроса");
    }
    m_stmt.reset(stmt);
}

Statement &Statement::bind(int index, std::int64_t value) {
    if (sqlite3_bind_int64(m_stmt.get(), index, value) != SQLITE_OK) {
        fail(m_db, "ошибка привязки параметра");
    }
    return *this;
}

Statement &Statement::bind(int index, std::string_view value) {
    if (sqlite3_bind_text(m_stmt.get(), index, value.data(), static_cast<int>(value.size()), SQLITE_TRANSIENT) != SQLITE_OK) {
        fail(m_db, "ошибка привязки параметра");
    }
    return *this;
}

Statement &Statement::bind_null(int index) {
    if (sqlite3_bind_null(m_stmt.get(), index) != SQLITE_OK) {
        fail(m_db, "ошибка привязки параметра");
    }
    return *this;
}

Statement &Statement::bind_blob(int index, std::string_view data) {
    if (sqlite3_bind_blob(m_stmt.get(), index, data.data(), static_cast<int>(data.size()), SQLITE_TRANSIENT) != SQLITE_OK) {
        fail(m_db, "ошибка привязки параметра");
    }
    return *this;
}

bool Statement::step() {
    auto const rc = sqlite3_step(m_stmt.get());
    if (rc == SQLITE_ROW) {
        return true;
    }
    if (rc == SQLITE_DONE) {
        return false;
    }
    fail(m_db, "ошибка выполнения запроса");
}

void Statement::reset() {
    sqlite3_reset(m_stmt.get());
    sqlite3_clear_bindings(m_stmt.get());
}

std::int64_t Statement::column_int(int column) const { return sqlite3_column_int64(m_stmt.get(), column); }

std::string Statement::column_text(int column) const {
    auto const *text = sqlite3_column_text(m_stmt.get(), column);
    if (text == nullptr) {
        return {};
    }
    return {reinterpret_cast<char const *>(text), static_cast<std::size_t>(sqlite3_column_bytes(m_stmt.get(), column))};
}

bool Statement::column_is_null(int column) const { return sqlite3_column_type(m_stmt.get(), column) == SQLITE_NULL; }

std::string Statement::column_blob(int column) const {
    auto const *data = static_cast<char const *>(sqlite3_column_blob(m_stmt.get(), column));
    if (data == nullptr) {
        return {};
    }
    return {data, static_cast<std::size_t>(sqlite3_column_bytes(m_stmt.get(), column))};
}

void Database::Closer::operator()(sqlite3 *db) const noexcept { sqlite3_close_v2(db); }

Database::Database(std::string const &path) {
    sqlite3 *db = nullptr;
    auto const rc = sqlite3_open_v2(path.c_str(), &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, nullptr);
    m_db.reset(db);
    if (rc != SQLITE_OK) {
        fail(db, "не удалось открыть базу " + path);
    }
    sqlite3_busy_timeout(db, 5000);
    exec("PRAGMA foreign_keys = ON; PRAGMA journal_mode = WAL; PRAGMA synchronous = NORMAL;");
}

void Database::exec(std::string_view sql) {
    std::string const text{sql};
    char *err = nullptr;
    if (sqlite3_exec(m_db.get(), text.c_str(), nullptr, nullptr, &err) != SQLITE_OK) {
        std::string msg = err != nullptr ? err : sqlite3_errmsg(m_db.get());
        sqlite3_free(err);
        throw DbError("ошибка выполнения SQL: " + msg);
    }
}

Statement Database::prepare(std::string_view sql) { return Statement{m_db.get(), sql}; }

std::int64_t Database::last_insert_id() const { return sqlite3_last_insert_rowid(m_db.get()); }

std::unique_lock<std::mutex> Database::lock() { return std::unique_lock{m_mutex}; }

Transaction::Transaction(Database &db) : m_db{db} { m_db.exec("BEGIN IMMEDIATE"); }

Transaction::~Transaction() {
    if (!m_done) {
        try {
            m_db.exec("ROLLBACK");
        } catch (...) {
        }
    }
}

void Transaction::commit() {
    m_db.exec("COMMIT");
    m_done = true;
}
