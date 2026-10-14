#pragma once
#include <cstdint>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>

struct sqlite3;
struct sqlite3_stmt;

/**
 * @brief Ошибка работы с базой; текст содержит сообщение SQLite.
 */
class DbError : public std::runtime_error {
   public:
    using std::runtime_error::runtime_error;
};

class StatementCache;

/**
 * @brief Подготовленный запрос SQLite.
 *
 * Параметры нумеруются с 1, колонки результата с 0, как в API SQLite. Запрос берётся из кеша соединения
 * и возвращается в него при уничтожении (сброшенным), поэтому повторная подготовка того же текста дешёвая.
 */
class Statement {
   public:
    Statement(sqlite3_stmt *stmt, sqlite3 *db, StatementCache *cache, std::string sql) noexcept;
    ~Statement();
    Statement(Statement &&other) noexcept;
    Statement(Statement const &) = delete;
    Statement &operator=(Statement const &) = delete;
    Statement &operator=(Statement &&) = delete;

    Statement &bind(int index, std::int64_t value);
    Statement &bind(int index, std::string_view value);
    Statement &bind_null(int index);
    /// Двоичные данные (BLOB).
    Statement &bind_blob(int index, std::string_view data);

    /**
     * @brief Выполняет очередной шаг запроса.
     * @return true, если получена строка результата; false, если запрос завершён.
     */
    bool step();

    /// Сбрасывает запрос и параметры для повторного выполнения.
    void reset();

    [[nodiscard]] std::int64_t column_int(int column) const;
    [[nodiscard]] std::string column_text(int column) const;
    [[nodiscard]] bool column_is_null(int column) const;
    /// Двоичные данные колонки (BLOB); пустая строка для NULL.
    [[nodiscard]] std::string column_blob(int column) const;

   private:
    std::string m_sql;
    sqlite3_stmt *m_stmt;
    sqlite3 *m_db;
    StatementCache *m_cache; ///< куда вернуть запрос; nullptr - финализировать
};

/**
 * @brief Соединение с базой SQLite.
 *
 * При открытии включает внешние ключи, WAL и ожидание блокировки.
 * Сервер обрабатывает запросы в нескольких потоках, поэтому обращения к базе
 * сериализуются мьютексом из lock().
 */
class Database {
   public:
    /**
     * @param path путь к файлу базы, ":memory:" для базы в памяти
     */
    explicit Database(std::string const &path);
    ~Database();
    Database(Database const &) = delete;
    Database &operator=(Database const &) = delete;

    /// Выполняет один или несколько SQL-операторов без результата.
    void exec(std::string_view sql);

    [[nodiscard]] Statement prepare(std::string_view sql);

    [[nodiscard]] std::int64_t last_insert_id() const;

    /// Захватывает базу на время обработки запроса.
    [[nodiscard]] std::unique_lock<std::mutex> lock();

    /// Идёт транзакция (соединение не в режиме автофиксации).
    [[nodiscard]] bool in_transaction() const;

   private:
    struct Closer {
        void operator()(sqlite3 *db) const noexcept;
    };

    std::mutex m_mutex;
    std::unique_ptr<sqlite3, Closer> m_db;
    std::unique_ptr<StatementCache> m_cache; ///< свободные подготовленные запросы по тексту
};

/**
 * @brief Транзакция с откатом в деструкторе, если не было commit().
 *
 * Внешняя начинается с BEGIN IMMEDIATE, чтобы блокировка на запись бралась сразу. Внутри уже идущей
 * транзакции - точка сохранения (SAVEPOINT): commit() её отпускает, откат возвращает только её
 * изменения, а итог фиксирует внешняя транзакция.
 */
class Transaction {
   public:
    explicit Transaction(Database &db);
    ~Transaction();
    Transaction(Transaction const &) = delete;
    Transaction &operator=(Transaction const &) = delete;

    void commit();

   private:
    std::string m_savepoint; ///< имя точки сохранения; пустое - внешняя транзакция
    Database &m_db;
    bool m_done = false;
};
