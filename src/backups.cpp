#include "backups.hpp"

#include <algorithm>
#include <charconv>
#include <ctime>
#include <regex>
#include <system_error>

#include "docs.hpp"
#include "errors.hpp"

namespace fs = std::filesystem;

namespace {

constexpr char const *KEY_ENABLED = "backup_enabled";
constexpr char const *KEY_TIME = "backup_time";
constexpr char const *KEY_KEEP = "backup_keep";
constexpr char const *KEY_LAST = "backup_last";
constexpr char const *KEY_ERROR = "backup_error";
constexpr int MAX_KEEP = 365;

/// Имя копии: bakery-20261014-030000-auto.db; группы - дата, время, вид.
std::regex const NAME_RE{R"(^bakery-(\d{4})(\d{2})(\d{2})-(\d{2})(\d{2})(\d{2})-(auto|manual)\.db$)"};

std::optional<Backup> parse_name(std::string const &name) {
    std::smatch m;
    if (!std::regex_match(name, m, NAME_RE)) {
        return std::nullopt;
    }
    Backup b;
    b.name = name;
    b.created = m[1].str() + "-" + m[2].str() + "-" + m[3].str() + " " + m[4].str() + ":" + m[5].str() + ":" + m[6].str();
    b.scheduled = m[7] == "auto";
    return b;
}

} // namespace

std::string local_now() {
    auto const t = std::time(nullptr);
    std::tm tm{};
    localtime_r(&t, &tm);
    char text[20];
    std::strftime(text, sizeof text, "%Y-%m-%d %H:%M:%S", &tm);
    return text;
}

Backups::Backups(Database &db, fs::path dir) : m_dir{std::move(dir)}, m_db{db} {}

std::vector<Backup> Backups::list() const {
    std::vector<Backup> out;
    std::error_code ec;
    for (auto const &entry : fs::directory_iterator(m_dir, ec)) {
        auto backup = parse_name(entry.path().filename().string());
        if (backup && entry.is_regular_file(ec)) {
            backup->size = entry.file_size(ec);
            out.push_back(std::move(*backup));
        }
    }
    std::sort(out.begin(), out.end(), [](auto const &a, auto const &b) { return a.name.substr(7, 15) > b.name.substr(7, 15); });
    return out;
}

Backup Backups::create(bool scheduled, std::string const &now) {
    std::string stamp;
    for (auto const c : now) {
        if (c >= '0' && c <= '9') {
            stamp += c;
        }
    }
    if (stamp.size() != 14) {
        throw UserError("неверный момент копии: " + now);
    }
    auto const name = "bakery-" + stamp.substr(0, 8) + "-" + stamp.substr(8) + (scheduled ? "-auto" : "-manual") + ".db";
    std::error_code ec;
    fs::create_directories(m_dir, ec);
    if (ec) {
        throw UserError("не удалось создать каталог копий " + m_dir.string() + ": " + ec.message());
    }
    auto const target = m_dir / name;
    if (fs::exists(target)) {
        throw UserError("копия " + name + " уже есть: подождите секунду");
    }
    auto const tmp = m_dir / (name + ".tmp");
    fs::remove(tmp, ec);
    try {
        auto stmt = m_db.prepare("VACUUM INTO ?");
        stmt.bind(1, tmp.string());
        stmt.step();
    } catch (DbError const &e) {
        fs::remove(tmp, ec);
        throw UserError("не удалось сделать копию в " + m_dir.string() + ": " + e.what());
    }
    fs::rename(tmp, target, ec);
    if (ec) {
        fs::remove(tmp, ec);
        throw UserError("не удалось сохранить копию " + name + ": " + ec.message());
    }
    auto backup = *parse_name(name);
    backup.size = fs::file_size(target, ec);
    return backup;
}

fs::path Backups::path(std::string const &name) const {
    if (!parse_name(name) || !fs::is_regular_file(m_dir / name)) {
        throw NotFoundError("нет копии " + name);
    }
    return m_dir / name;
}

void Backups::remove(std::string const &name) {
    std::error_code ec;
    if (!fs::remove(path(name), ec) || ec) {
        throw UserError("не удалось удалить копию " + name + (ec ? ": " + ec.message() : ""));
    }
}

BackupSchedule Backups::schedule() const {
    BackupSchedule s;
    s.enabled = setting(KEY_ENABLED, "0") == "1";
    s.time = setting(KEY_TIME, s.time);
    auto const keep = setting(KEY_KEEP, std::to_string(s.keep));
    std::from_chars(keep.data(), keep.data() + keep.size(), s.keep);
    s.last_run = setting(KEY_LAST, "");
    s.last_error = setting(KEY_ERROR, "");
    return s;
}

void Backups::set_schedule(BackupSchedule const &schedule) {
    auto const time = normalize_time(schedule.time).substr(0, 5);
    if (schedule.keep < 1 || schedule.keep > MAX_KEEP) {
        throw UserError("хранить можно от 1 до " + std::to_string(MAX_KEEP) + " копий, указано " + std::to_string(schedule.keep));
    }
    Transaction tx{m_db};
    set_setting(KEY_ENABLED, schedule.enabled ? "1" : "0");
    set_setting(KEY_TIME, time);
    set_setting(KEY_KEEP, std::to_string(schedule.keep));
    tx.commit();
}

std::optional<Backup> Backups::run_due(std::string const &now) {
    auto const s = schedule();
    auto const today = now.substr(0, 10);
    if (!s.enabled || now.substr(11, 5) < s.time || s.last_run == today) {
        return std::nullopt;
    }
    set_setting(KEY_LAST, today);
    try {
        auto backup = create(true, now);
        set_setting(KEY_ERROR, "");
        prune(s.keep);
        return backup;
    } catch (UserError const &e) {
        set_setting(KEY_ERROR, e.what());
        throw;
    }
}

void Backups::prune(int keep) {
    int kept = 0;
    std::error_code ec;
    for (auto const &b : list()) {
        if (b.scheduled && ++kept > keep) {
            fs::remove(m_dir / b.name, ec);
        }
    }
}

std::string Backups::setting(std::string const &key, std::string const &fallback) const {
    auto stmt = m_db.prepare("SELECT value FROM settings WHERE key = ?");
    stmt.bind(1, key);
    return stmt.step() ? stmt.column_text(0) : fallback;
}

void Backups::set_setting(std::string const &key, std::string const &value) {
    auto stmt = m_db.prepare("INSERT INTO settings (key, value) VALUES (?, ?) ON CONFLICT (key) DO UPDATE SET value = excluded.value");
    stmt.bind(1, key).bind(2, value);
    stmt.step();
}
