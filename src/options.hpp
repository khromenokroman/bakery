#pragma once
#include <string>
#include <string_view>
#include <vector>

/**
 * @brief Параметры запуска сервера.
 */
struct Options {
    std::string db_path = "bakery.db";    ///< файл базы SQLite
    std::string host = "127.0.0.1";       ///< адрес, на котором слушает сервер
    std::string web_dir = BAKERY_WEB_DIR; ///< каталог со страницами веб-интерфейса
    std::string backup_dir;               ///< каталог резервных копий; пустой - backups рядом с базой
    int port = 8080;                      ///< порт HTTP
    bool help = false;
    bool version = false;
};

/**
 * @brief Разбирает аргументы командной строки (без имени программы).
 *
 * @throws std::invalid_argument при неизвестном ключе, отсутствии значения или неверном порте
 */
[[nodiscard]] Options parse_options(std::vector<std::string_view> const &args);

/**
 * @brief Текст справки по ключам.
 */
[[nodiscard]] std::string usage();

/**
 * @brief Каталог резервных копий: заданный ключом --backup-dir или backups рядом с файлом базы.
 */
[[nodiscard]] std::string backup_dir(Options const &opt);
