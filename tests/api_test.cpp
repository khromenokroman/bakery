#include "api.hpp"

#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <httplib.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <nlohmann/json.hpp>
#include <thread>

#include "db.hpp"
#include "schema.hpp"

using testing::HasSubstr;

namespace {

/**
 * @brief Сервер API на случайном локальном порте, работающий в отдельном потоке.
 */
class TestServer {
   public:
    explicit TestServer(std::string const &web_dir = {}) : m_db{":memory:"} {
        migrate(m_db);
        m_api = std::make_unique<Api>(m_db, web_dir);
        m_api->register_routes(m_server);
        m_port = m_server.bind_to_any_port("127.0.0.1");
        m_thread = std::thread{[this] { m_server.listen_after_bind(); }};
        while (!m_server.is_running()) {
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
        }
    }

    ~TestServer() {
        m_server.stop();
        m_thread.join();
    }

    [[nodiscard]] httplib::Client client() const { return httplib::Client{"127.0.0.1", m_port}; }

   private:
    httplib::Server m_server;
    Database m_db;
    std::unique_ptr<Api> m_api;
    std::thread m_thread;
    int m_port = 0;
};

} // namespace

/**
 * @brief /api/version отдаёт версию программы и версию схемы базы.
 */
TEST(Api, Version) {
    TestServer srv;
    auto const res = srv.client().Get("/api/version");
    ASSERT_TRUE(res) << "нет ответа: " << httplib::to_string(res.error());
    EXPECT_EQ(res->status, 200) << res->body;
    EXPECT_THAT(res->get_header_value("Content-Type"), HasSubstr("application/json"));
    auto const body = nlohmann::json::parse(res->body);
    EXPECT_EQ(body.at("version"), BAKERY_VERSION) << res->body;
    EXPECT_EQ(body.at("schema"), schema_version()) << res->body;
}

/**
 * @brief Неизвестный метод API отвечает 404 с текстом ошибки в JSON, а не пустым телом.
 */
TEST(Api, UnknownMethodIsJsonError) {
    TestServer srv;
    auto const res = srv.client().Get("/api/nothing");
    ASSERT_TRUE(res) << "нет ответа: " << httplib::to_string(res.error());
    EXPECT_EQ(res->status, 404);
    auto const body = nlohmann::json::parse(res->body);
    EXPECT_THAT(body.at("error").get<std::string>(), HasSubstr("/api/nothing")) << res->body;
}

/**
 * @brief Страницы веб-интерфейса раздаются из каталога web_dir.
 */
TEST(Api, ServesWebDir) {
    auto const dir = std::filesystem::temp_directory_path() / ("bakery-web-" + std::to_string(::getpid()));
    std::filesystem::create_directories(dir);
    std::ofstream{dir / "index.html"} << "<p>пекарня</p>";
    {
        TestServer srv{dir.string()};
        auto const res = srv.client().Get("/");
        ASSERT_TRUE(res) << "нет ответа: " << httplib::to_string(res.error());
        EXPECT_EQ(res->status, 200);
        EXPECT_EQ(res->body, "<p>пекарня</p>") << "ожидалось содержимое " << (dir / "index.html");
    }
    std::filesystem::remove_all(dir);
}

/**
 * @brief Несуществующий каталог веб-интерфейса обнаруживается при запуске, а не при первом запросе.
 */
TEST(Api, MissingWebDirFails) {
    Database db{":memory:"};
    Api api{db, "/nonexistent/bakery-web"};
    httplib::Server server;
    try {
        api.register_routes(server);
        FAIL() << "ожидался отказ для несуществующего каталога";
    } catch (std::runtime_error const &e) {
        EXPECT_THAT(std::string(e.what()), HasSubstr("/nonexistent/bakery-web"));
    }
}
