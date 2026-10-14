#include "db.hpp"

#include <sqlite3.h>

#include <cstdint>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

[[noreturn]] void fail(sqlite3 *db, std::string_view what) {
    throw DbError(std::string(what) + ": " + (db != nullptr ? sqlite3_errmsg(db) : "нет соединения"));
}

} // namespace

/**
 * @brief Свободные подготовленные запросы соединения по тексту SQL.
 *
 * Запрос выдаётся одному Statement; пока он занят (вложенный вызов с тем же текстом), готовится ещё один.
 * Размер ограничен: запросы сверх предела просто финализируются.
 */
class StatementCache {
   public:
    StatementCache() = default;
    StatementCache(StatementCache const &) = delete;
    StatementCache &operator=(StatementCache const &) = delete;
    ~StatementCache() {
        for (auto &[sql, list] : m_free) {
            for (auto *stmt : list) {
                sqlite3_finalize(stmt);
            }
        }
    }

    sqlite3_stmt *take(std::string const &sql) {
        auto const it = m_free.find(sql);
        if (it == m_free.end() || it->second.empty()) {
            return nullptr;
        }
        auto *stmt = it->second.back();
        it->second.pop_back();
        return stmt;
    }

    void give(std::string &&sql, sqlite3_stmt *stmt) {
        sqlite3_reset(stmt);
        sqlite3_clear_bindings(stmt);
        auto &list = m_free[std::move(sql)];
        if (list.size() >= MAX_SAME || m_free.size() > MAX_TEXTS) {
            sqlite3_finalize(stmt);
            return;
        }
        list.push_back(stmt);
    }

   private:
    static constexpr std::size_t MAX_SAME = 4;    ///< одинаковых запросов (вложенные вызовы)
    static constexpr std::size_t MAX_TEXTS = 512; ///< разных текстов

    std::unordered_map<std::string, std::vector<sqlite3_stmt *>> m_free;
};

Statement::Statement(sqlite3_stmt *stmt, sqlite3 *db, StatementCache *cache, std::string sql) noexcept
    : m_sql{std::move(sql)}, m_stmt{stmt}, m_db{db}, m_cache{cache} {}

Statement::~Statement() {
    if (m_stmt == nullptr) {
        return;
    }
    if (m_cache != nullptr) {
        m_cache->give(std::move(m_sql), m_stmt);
    } else {
        sqlite3_finalize(m_stmt);
    }
}

Statement::Statement(Statement &&other) noexcept : m_sql{std::move(other.m_sql)}, m_stmt{other.m_stmt}, m_db{other.m_db}, m_cache{other.m_cache} {
    other.m_stmt = nullptr;
}

Statement &Statement::bind(int index, std::int64_t value) {
    if (sqlite3_bind_int64(m_stmt, index, value) != SQLITE_OK) {
        fail(m_db, "ошибка привязки параметра");
    }
    return *this;
}

Statement &Statement::bind(int index, std::string_view value) {
    if (sqlite3_bind_text(m_stmt, index, value.data(), static_cast<int>(value.size()), SQLITE_TRANSIENT) != SQLITE_OK) {
        fail(m_db, "ошибка привязки параметра");
    }
    return *this;
}

Statement &Statement::bind_null(int index) {
    if (sqlite3_bind_null(m_stmt, index) != SQLITE_OK) {
        fail(m_db, "ошибка привязки параметра");
    }
    return *this;
}

Statement &Statement::bind_blob(int index, std::string_view data) {
    if (sqlite3_bind_blob(m_stmt, index, data.data(), static_cast<int>(data.size()), SQLITE_TRANSIENT) != SQLITE_OK) {
        fail(m_db, "ошибка привязки параметра");
    }
    return *this;
}

bool Statement::step() {
    auto const rc = sqlite3_step(m_stmt);
    if (rc == SQLITE_ROW) {
        return true;
    }
    if (rc == SQLITE_DONE) {
        return false;
    }
    fail(m_db, "ошибка выполнения запроса");
}

void Statement::reset() {
    sqlite3_reset(m_stmt);
    sqlite3_clear_bindings(m_stmt);
}

std::int64_t Statement::column_int(int column) const { return sqlite3_column_int64(m_stmt, column); }

std::string Statement::column_text(int column) const {
    auto const *text = sqlite3_column_text(m_stmt, column);
    if (text == nullptr) {
        return {};
    }
    return {reinterpret_cast<char const *>(text), static_cast<std::size_t>(sqlite3_column_bytes(m_stmt, column))};
}

bool Statement::column_is_null(int column) const { return sqlite3_column_type(m_stmt, column) == SQLITE_NULL; }

std::string Statement::column_blob(int column) const {
    auto const *data = static_cast<char const *>(sqlite3_column_blob(m_stmt, column));
    if (data == nullptr) {
        return {};
    }
    return {data, static_cast<std::size_t>(sqlite3_column_bytes(m_stmt, column))};
}

void Database::Closer::operator()(sqlite3 *db) const noexcept { sqlite3_close_v2(db); }

Database::~Database() {
    // подготовленные запросы финализируются до закрытия соединения
    m_cache.reset();
}

Database::Database(std::string const &path) : m_cache{std::make_unique<StatementCache>()} {
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

Statement Database::prepare(std::string_view sql) {
    std::string text{sql};
    auto *stmt = m_cache->take(text);
    if (stmt == nullptr &&
        sqlite3_prepare_v3(m_db.get(), text.data(), static_cast<int>(text.size()), SQLITE_PREPARE_PERSISTENT, &stmt, nullptr) != SQLITE_OK) {
        fail(m_db.get(), "ошибка подготовки запроса");
    }
    return Statement{stmt, m_db.get(), m_cache.get(), std::move(text)};
}

std::int64_t Database::last_insert_id() const { return sqlite3_last_insert_rowid(m_db.get()); }

std::unique_lock<std::mutex> Database::lock() { return std::unique_lock{m_mutex}; }

bool Database::in_transaction() const { return sqlite3_get_autocommit(m_db.get()) == 0; }

Transaction::Transaction(Database &db) : m_db{db} {
    if (m_db.in_transaction()) {
        // имя уникально среди одновременно открытых: адрес объекта на стеке
        m_savepoint = "tx" + std::to_string(reinterpret_cast<std::uintptr_t>(this));
        m_db.exec("SAVEPOINT " + m_savepoint);
    } else {
        m_db.exec("BEGIN IMMEDIATE");
    }
}

Transaction::~Transaction() {
    if (!m_done) {
        try {
            if (m_savepoint.empty()) {
                m_db.exec("ROLLBACK");
            } else {
                m_db.exec("ROLLBACK TO " + m_savepoint + "; RELEASE " + m_savepoint);
            }
        } catch (...) {
        }
    }
}

void Transaction::commit() {
    m_db.exec(m_savepoint.empty() ? "COMMIT" : "RELEASE " + m_savepoint);
    m_done = true;
}
