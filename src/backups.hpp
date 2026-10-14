#pragma once
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "db.hpp"

/**
 * @brief Файл резервной копии в каталоге копий.
 */
struct Backup {
    std::string name;       ///< имя файла: bakery-ГГГГММДД-ЧЧММСС-auto.db или -manual.db
    std::string created;    ///< "ГГГГ-ММ-ДД ЧЧ:ММ:СС" местного времени, из имени
    std::uintmax_t size{};  ///< байт
    bool scheduled = false; ///< сделана по расписанию (их удаляет хранение последних N)
};

/**
 * @brief Расписание копий: ежедневно в заданное время, хранить последние keep копий по расписанию.
 */
struct BackupSchedule {
    std::string time = "03:00"; ///< ЧЧ:ММ местного времени
    std::string last_run; ///< только для чтения: дата последней попытки по расписанию, ГГГГ-ММ-ДД
    std::string last_error; ///< только для чтения: ошибка последней попытки, пусто - успешно
    int keep = 14;          ///< сколько копий по расписанию хранить; ручные не удаляются
    bool enabled = false;
};

/**
 * @brief Резервные копии базы в отдельном каталоге.
 *
 * Копия делается командой VACUUM INTO: целостный снимок без остановки работы, на время копии база
 * захвачена вызывающим. Копия пишется во временный файл и переименовывается, поэтому в списке не бывает
 * недописанных копий. Расписание хранится в таблице settings (ключи backup_*), вне общих реквизитов.
 * Методы не захватывают блокировку базы.
 */
class Backups {
   public:
    /**
     * @param dir каталог копий; создаётся при первой копии
     */
    Backups(Database &db, std::filesystem::path dir);

    /// Каталог копий.
    [[nodiscard]] std::filesystem::path const &dir() const { return m_dir; }

    /// Копии, новые первыми; файлы с чужими именами не показываются.
    [[nodiscard]] std::vector<Backup> list() const;

    /**
     * @brief Делает копию сейчас.
     * @param scheduled копия по расписанию (иначе ручная)
     * @param now момент "ГГГГ-ММ-ДД ЧЧ:ММ:СС" для имени файла
     * @throws UserError если копия с таким именем уже есть или каталог недоступен
     */
    Backup create(bool scheduled, std::string const &now);

    /**
     * @brief Путь к файлу копии для скачивания.
     * @throws NotFoundError неверное имя или нет такой копии
     */
    [[nodiscard]] std::filesystem::path path(std::string const &name) const;

    /// @throws NotFoundError неверное имя или нет такой копии
    void remove(std::string const &name);

    [[nodiscard]] BackupSchedule schedule() const;

    /// @throws UserError неверное время или число копий (1..365)
    void set_schedule(BackupSchedule const &schedule);

    /**
     * @brief Делает копию по расписанию, если пора: расписание включено, время на сегодня наступило,
     * а сегодня копии по расписанию ещё не было. Затем удаляет лишние старые копии по расписанию.
     * @param now момент "ГГГГ-ММ-ДД ЧЧ:ММ:СС" местного времени
     * Попытка запоминается и при ошибке (last_error): повтор будет только завтра, а не каждые полминуты.
     * @return сделанная копия или nullopt
     * @throws UserError если копия не удалась
     */
    std::optional<Backup> run_due(std::string const &now);

   private:
    /// Удаляет копии по расписанию сверх keep, старые первыми.
    void prune(int keep);

    [[nodiscard]] std::string setting(std::string const &key, std::string const &fallback) const;
    void set_setting(std::string const &key, std::string const &value);

    std::filesystem::path m_dir;
    Database &m_db;
};

/// Текущий момент "ГГГГ-ММ-ДД ЧЧ:ММ:СС" местного времени.
[[nodiscard]] std::string local_now();
