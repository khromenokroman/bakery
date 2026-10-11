#include "options.hpp"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <stdexcept>
#include <string>

using testing::HasSubstr;

namespace {

/// Ожидает отказ разбора аргументов ... с текстом, содержащим reason.
#define EXPECT_PARSE_FAILS(reason, ...)                                          \
    do {                                                                         \
        SCOPED_TRACE("аргументы: " #__VA_ARGS__);                                \
        try {                                                                    \
            (void)parse_options({__VA_ARGS__});                                  \
            ADD_FAILURE() << "ожидался отказ с причиной \"" << (reason) << "\""; \
        } catch (std::invalid_argument const &e) {                               \
            EXPECT_THAT(std::string(e.what()), HasSubstr(reason));               \
        }                                                                        \
    } while (false)

} // namespace

/**
 * @brief Без аргументов действуют значения по умолчанию: локальный адрес, порт 8080, bakery.db.
 */
TEST(Options, Defaults) {
    auto const opt = parse_options({});
    EXPECT_EQ(opt.db_path, "bakery.db") << "файл базы по умолчанию";
    EXPECT_EQ(opt.host, "127.0.0.1") << "по умолчанию сервер не должен быть виден в сети";
    EXPECT_EQ(opt.port, 8080) << "порт по умолчанию";
    EXPECT_EQ(opt.web_dir, BAKERY_WEB_DIR) << "каталог веб-интерфейса по умолчанию";
    EXPECT_FALSE(opt.help);
    EXPECT_FALSE(opt.version);
}

/**
 * @brief Все ключи со значениями попадают в соответствующие поля.
 */
TEST(Options, AllKeys) {
    auto const opt = parse_options({"--db", "/var/lib/bakery/b.db", "--host", "0.0.0.0", "--port", "9000", "--web", "/tmp/web"});
    EXPECT_EQ(opt.db_path, "/var/lib/bakery/b.db");
    EXPECT_EQ(opt.host, "0.0.0.0");
    EXPECT_EQ(opt.port, 9000);
    EXPECT_EQ(opt.web_dir, "/tmp/web");
}

/**
 * @brief Короткие и длинные ключи справки и версии.
 */
TEST(Options, HelpAndVersion) {
    EXPECT_TRUE(parse_options({"-h"}).help) << "-h";
    EXPECT_TRUE(parse_options({"--help"}).help) << "--help";
    EXPECT_TRUE(parse_options({"-v"}).version) << "-v";
    EXPECT_TRUE(parse_options({"--version"}).version) << "--version";
}

/**
 * @brief Ошибки разбора сообщают, что именно не так.
 */
TEST(Options, Errors) {
    EXPECT_PARSE_FAILS("неизвестный ключ: --bogus", "--bogus");
    EXPECT_PARSE_FAILS("не указано значение ключа --db", "--db");
    EXPECT_PARSE_FAILS("неверный порт: 0", "--port", "0");
    EXPECT_PARSE_FAILS("неверный порт: 65536", "--port", "65536");
    EXPECT_PARSE_FAILS("неверный порт: 80a", "--port", "80a");
}
