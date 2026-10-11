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
        EXPECT_THAT(res->body, testing::StrEq("<p>пекарня</p>")) << "ожидалось содержимое " << (dir / "index.html");
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

namespace {

/// Ответ API: код и разобранное тело.
struct Reply {
    nlohmann::json body;
    int status = 0;
};

/**
 * @brief Выполняет запрос и разбирает JSON-ответ; при сбое соединения тест падает с причиной.
 */
Reply call(TestServer const &srv, std::string const &method, std::string const &path, nlohmann::json const &body = nullptr) {
    auto cli = srv.client();
    httplib::Result res;
    auto const text = body.is_null() ? std::string{} : body.dump();
    if (method == "GET") {
        res = cli.Get(path);
    } else if (method == "POST") {
        res = cli.Post(path, text, "application/json");
    } else if (method == "PUT") {
        res = cli.Put(path, text, "application/json");
    } else {
        res = cli.Delete(path);
    }
    if (!res) {
        ADD_FAILURE() << method << " " << path << ": нет ответа: " << httplib::to_string(res.error());
        return {};
    }
    auto parsed = nlohmann::json::parse(res->body, nullptr, false);
    if (parsed.is_discarded()) {
        ADD_FAILURE() << method << " " << path << ": ответ не JSON: " << res->body;
    }
    return {parsed, res->status};
}

/// Вызов с трассировкой: при падении видно запрос и строку теста.
#define CALL(...)                   \
    [&] {                           \
        SCOPED_TRACE(#__VA_ARGS__); \
        return call(__VA_ARGS__);   \
    }()

} // namespace

/**
 * @brief Создание, чтение, изменение и удаление позиции через API.
 */
TEST(ApiCatalog, ItemCrud) {
    TestServer srv;
    auto const created = CALL(srv, "POST", "/api/items", {{"name", "Батон"}, {"kind", "product"}, {"unit", "pcs"}});
    ASSERT_EQ(created.status, 201) << created.body;
    auto const id = created.body.at("id").get<std::int64_t>();
    EXPECT_EQ(created.body.at("name"), "Батон");
    EXPECT_EQ(created.body.at("has_recipe"), false);

    auto const path = "/api/items/" + std::to_string(id);
    auto const updated = CALL(srv, "PUT", path, {{"archived", true}});
    ASSERT_EQ(updated.status, 200) << updated.body;
    EXPECT_EQ(updated.body.at("archived"), true);
    EXPECT_EQ(updated.body.at("unit"), "pcs") << "поля, которых нет в запросе, не должны меняться";

    EXPECT_EQ(CALL(srv, "GET", "/api/items").body.size(), 0U) << "архивная позиция скрыта по умолчанию";
    EXPECT_EQ(CALL(srv, "GET", "/api/items?archived=1").body.size(), 1U);

    EXPECT_EQ(CALL(srv, "DELETE", path).status, 200);
    EXPECT_EQ(CALL(srv, "GET", path).status, 404) << "после удаления";
}

/**
 * @brief Ошибки данных возвращаются кодом 400 с причиной, несуществующие объекты кодом 404.
 */
TEST(ApiCatalog, Errors) {
    TestServer srv;
    auto r = CALL(srv, "POST", "/api/items", {{"name", "Мука"}, {"kind", "flour"}, {"unit", "kg"}});
    EXPECT_EQ(r.status, 400);
    EXPECT_THAT(r.body.value("error", ""), HasSubstr("неизвестный вид номенклатуры: flour"));

    r = CALL(srv, "POST", "/api/items", {{"name", "Мука"}, {"kind", "raw"}});
    EXPECT_EQ(r.status, 400);
    EXPECT_THAT(r.body.value("error", ""), HasSubstr("нет поля unit"));

    r = CALL(srv, "POST", "/api/items", {{"name", 5}, {"kind", "raw"}, {"unit", "kg"}});
    EXPECT_THAT(r.body.value("error", ""), HasSubstr("поле name должно быть строкой"));

    auto res = srv.client().Post("/api/items", "{не json", "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 400) << res->body;
    EXPECT_THAT(res->body, HasSubstr("не является JSON"));

    r = CALL(srv, "GET", "/api/items/999");
    EXPECT_EQ(r.status, 404);
    EXPECT_THAT(r.body.value("error", ""), HasSubstr("id 999"));
    EXPECT_EQ(CALL(srv, "GET", "/api/items/abc").status, 404);
    EXPECT_EQ(CALL(srv, "GET", "/api/items?kind=bad").status, 400);
}

/**
 * @brief Рецептура через API: нет → null, сохранение, чтение с названиями, удаление.
 */
TEST(ApiCatalog, Recipe) {
    TestServer srv;
    auto const flour = CALL(srv, "POST", "/api/items", {{"name", "Мука"}, {"kind", "raw"}, {"unit", "kg"}}).body.at("id");
    auto const dough = CALL(srv, "POST", "/api/items", {{"name", "Тесто"}, {"kind", "semi"}, {"unit", "kg"}}).body.at("id");
    auto const path = "/api/items/" + dough.dump() + "/recipe";

    auto r = CALL(srv, "GET", path);
    EXPECT_EQ(r.status, 200);
    EXPECT_TRUE(r.body.is_null()) << "у новой позиции рецептуры нет: " << r.body;

    nlohmann::json const recipe = {{"output_qty", 10000}, {"lines", {{{"component_id", flour}, {"qty", 6000}}}}};
    r = CALL(srv, "PUT", path, recipe);
    ASSERT_EQ(r.status, 200) << r.body;
    EXPECT_EQ(r.body.at("lines").at(0).at("name"), "Мука") << r.body;

    r = CALL(srv, "GET", path);
    EXPECT_EQ(r.body.at("output_qty"), 10000) << r.body;

    r = CALL(srv, "PUT", "/api/items/" + flour.dump() + "/recipe", recipe);
    EXPECT_EQ(r.status, 400);
    EXPECT_THAT(r.body.value("error", ""), HasSubstr("у сырья нет рецептуры"));

    EXPECT_EQ(CALL(srv, "DELETE", path).status, 200);
    EXPECT_TRUE(CALL(srv, "GET", path).body.is_null());
    EXPECT_EQ(CALL(srv, "GET", "/api/items/999/recipe").status, 404);
}

/**
 * @brief Заявка через API: создание, выборка по дате, изменение, расчёт потребности, удаление.
 */
TEST(ApiDocs, OrderAndRequirements) {
    TestServer srv;
    auto const flour = CALL(srv, "POST", "/api/items", {{"name", "Мука"}, {"kind", "raw"}, {"unit", "kg"}}).body.at("id");
    auto const bread = CALL(srv, "POST", "/api/items", {{"name", "Хлеб"}, {"kind", "product"}, {"unit", "pcs"}}).body.at("id");
    CALL(srv, "PUT", "/api/items/" + bread.dump() + "/recipe", {{"output_qty", 1000}, {"lines", {{{"component_id", flour}, {"qty", 300}}}}});

    nlohmann::json const order = {
        {"type", "order"}, {"date", "2026-10-12"}, {"comment", "Иванов"}, {"lines", {{{"item_id", bread}, {"qty", 10000}}}}};
    auto r = CALL(srv, "POST", "/api/docs", order);
    ASSERT_EQ(r.status, 201) << r.body;
    auto const id = r.body.at("id").get<std::int64_t>();
    EXPECT_EQ(r.body.at("number"), 1);
    EXPECT_EQ(r.body.at("lines").at(0).at("unit"), "pcs") << r.body;

    EXPECT_EQ(CALL(srv, "GET", "/api/docs?type=order&from=2026-10-12&to=2026-10-12").body.size(), 1U);
    EXPECT_EQ(CALL(srv, "GET", "/api/docs?type=order&from=2026-10-13").body.size(), 0U);

    auto changed = order;
    changed["lines"][0]["qty"] = 20000;
    r = CALL(srv, "PUT", "/api/docs/" + std::to_string(id), changed);
    ASSERT_EQ(r.status, 200) << r.body;

    r = CALL(srv, "GET", "/api/requirements?docs=" + std::to_string(id));
    ASSERT_EQ(r.status, 200) << r.body;
    EXPECT_EQ(r.body.at("raw").at(0).at("qty"), 6000) << "20 шт × 0,3 кг муки: " << r.body;
    EXPECT_TRUE(r.body.at("warnings").empty()) << r.body;

    EXPECT_EQ(CALL(srv, "DELETE", "/api/docs/" + std::to_string(id)).status, 200);
    EXPECT_EQ(CALL(srv, "GET", "/api/docs/" + std::to_string(id)).status, 404);
}

/**
 * @brief Ошибки API документов: нет вида, неверная дата фильтра, неверный и пустой список заявок.
 */
TEST(ApiDocs, Errors) {
    TestServer srv;
    auto r = CALL(srv, "POST", "/api/docs", {{"date", "2026-10-12"}, {"lines", nlohmann::json::array()}});
    EXPECT_EQ(r.status, 400);
    EXPECT_THAT(r.body.value("error", ""), HasSubstr("нет поля type"));

    r = CALL(srv, "GET", "/api/docs?from=вчера");
    EXPECT_EQ(r.status, 400);
    EXPECT_THAT(r.body.value("error", ""), HasSubstr("неверная дата"));

    r = CALL(srv, "GET", "/api/requirements?docs=1,x");
    EXPECT_EQ(r.status, 400);
    EXPECT_THAT(r.body.value("error", ""), HasSubstr("неверный список id"));

    r = CALL(srv, "GET", "/api/requirements");
    EXPECT_EQ(r.status, 400);
    EXPECT_THAT(r.body.value("error", ""), HasSubstr("не выбрано ни одной заявки"));
}
