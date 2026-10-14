#include "options.hpp"

#include <charconv>
#include <filesystem>
#include <stdexcept>

namespace {

int parse_port(std::string_view text) {
    int port = 0;
    auto const [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), port);
    if (ec != std::errc{} || ptr != text.data() + text.size() || port < 1 || port > 65535) {
        throw std::invalid_argument("неверный порт: " + std::string(text));
    }
    return port;
}

} // namespace

Options parse_options(std::vector<std::string_view> const &args) {
    Options opt;
    for (std::size_t i = 0; i < args.size(); ++i) {
        auto const key = args[i];
        if (key == "-h" || key == "--help") {
            opt.help = true;
            continue;
        }
        if (key == "-v" || key == "--version") {
            opt.version = true;
            continue;
        }
        if (key != "--db" && key != "--host" && key != "--port" && key != "--web" && key != "--backup-dir") {
            throw std::invalid_argument("неизвестный ключ: " + std::string(key));
        }
        if (i + 1 >= args.size()) {
            throw std::invalid_argument("не указано значение ключа " + std::string(key));
        }
        auto const value = args[++i];
        if (key == "--db") {
            opt.db_path = value;
        } else if (key == "--host") {
            opt.host = value;
        } else if (key == "--port") {
            opt.port = parse_port(value);
        } else if (key == "--backup-dir") {
            opt.backup_dir = value;
        } else {
            opt.web_dir = value;
        }
    }
    return opt;
}

std::string usage() {
    return "Использование: bakery [ключи]\n"
           "Веб-сервер учёта продукции пекарни.\n"
           "\n"
           "  --db ФАЙЛ      файл базы SQLite (по умолчанию bakery.db)\n"
           "  --host АДРЕС   адрес для входящих соединений (по умолчанию 127.0.0.1)\n"
           "  --port ПОРТ    порт HTTP (по умолчанию 8080)\n"
           "  --web КАТАЛОГ  каталог веб-интерфейса (по умолчанию " BAKERY_WEB_DIR
           ")\n"
           "  --backup-dir КАТАЛОГ  каталог резервных копий (по умолчанию backups рядом с базой)\n"
           "  -v, --version  показать версию\n"
           "  -h, --help     показать эту справку\n";
}

std::string backup_dir(Options const &opt) {
    if (!opt.backup_dir.empty()) {
        return opt.backup_dir;
    }
    return (std::filesystem::path(opt.db_path).parent_path() / "backups").string();
}
