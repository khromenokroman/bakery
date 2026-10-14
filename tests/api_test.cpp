#include "api.hpp"

#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <httplib.h>
#include <unistd.h>

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
        m_backup_dir = std::filesystem::temp_directory_path() /
                       ("bakery-api-backups-" + std::to_string(reinterpret_cast<std::uintptr_t>(this)) + "-" + std::to_string(::getpid()));
        std::filesystem::remove_all(m_backup_dir);
        m_api = std::make_unique<Api>(m_db, web_dir, m_backup_dir.string());
        m_api->register_routes(m_server);
        m_port = m_server.bind_to_any_port("127.0.0.1");
        m_thread = std::thread{[this] { m_server.listen_after_bind(); }};
        while (!m_server.is_running()) {
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
        }
        // первый пользователь - администратор; его сессия уходит со всеми запросами client()
        auto const res = anonymous().Post("/api/auth/setup", R"({"login": "admin", "name": "Админ", "password": "secret1"})", "application/json");
        if (res && res->status == 201) {
            m_cookie = cookie_of(*res);
        }
    }

    /// Пара "bakery_session=..." из Set-Cookie ответа.
    static std::string cookie_of(httplib::Response const &res) {
        auto const header = res.get_header_value("Set-Cookie");
        return header.substr(0, header.find(';'));
    }

    ~TestServer() {
        m_server.stop();
        m_thread.join();
        std::filesystem::remove_all(m_backup_dir);
    }

    /// Клиент с сессией администратора.
    [[nodiscard]] httplib::Client client() const { return client_as(m_cookie); }

    /// Клиент с заданной cookie сессии ("bakery_session=...").
    [[nodiscard]] httplib::Client client_as(std::string const &cookie) const {
        httplib::Client cli{"127.0.0.1", m_port};
        cli.set_default_headers({{"Cookie", cookie}});
        return cli;
    }

    /// Клиент без входа.
    [[nodiscard]] httplib::Client anonymous() const { return httplib::Client{"127.0.0.1", m_port}; }

   private:
    httplib::Server m_server;
    Database m_db;
    std::unique_ptr<Api> m_api;
    std::thread m_thread;
    std::filesystem::path m_backup_dir;
    std::string m_cookie;
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

/**
 * @brief Поступление через API: проведение, остатки на дату, отмена проведения, фильтр по нескольким видам.
 */
TEST(ApiStock, ProductionPostUnpost) {
    TestServer srv;
    auto const bread = CALL(srv, "POST", "/api/items", {{"name", "Батон"}, {"kind", "product"}, {"unit", "pcs"}}).body.at("id");
    auto r = CALL(srv, "POST", "/api/docs", {{"type", "production"}, {"date", "2026-10-12"}, {"lines", {{{"item_id", bread}, {"qty", 40000}}}}});
    ASSERT_EQ(r.status, 201) << r.body;
    auto const id = std::to_string(r.body.at("id").get<std::int64_t>());
    CALL(srv, "POST", "/api/docs", {{"type", "order"}, {"date", "2026-10-12"}, {"lines", {{{"item_id", bread}, {"qty", 1000}}}}});

    r = CALL(srv, "POST", "/api/docs/" + id + "/post");
    ASSERT_EQ(r.status, 200) << r.body;
    EXPECT_EQ(r.body.at("status"), "posted");

    r = CALL(srv, "GET", "/api/stock?date=2026-10-12");
    ASSERT_EQ(r.body.size(), 1U) << r.body;
    EXPECT_EQ(r.body.at(0).at("qty"), 40000);
    EXPECT_EQ(r.body.at(0).at("unit"), "pcs");
    EXPECT_TRUE(CALL(srv, "GET", "/api/stock?date=2026-10-11").body.empty()) << "до поступления";

    r = CALL(srv, "POST", "/api/docs/" + id + "/post");
    EXPECT_EQ(r.status, 400);
    EXPECT_THAT(r.body.value("error", ""), HasSubstr("уже проведён"));

    EXPECT_EQ(CALL(srv, "GET", "/api/docs?type=production,order").body.size(), 2U);
    EXPECT_EQ(CALL(srv, "GET", "/api/docs?type=production").body.size(), 1U);
    EXPECT_EQ(CALL(srv, "GET", "/api/docs?type=production,bogus").status, 400);

    r = CALL(srv, "POST", "/api/docs/" + id + "/unpost");
    EXPECT_EQ(r.body.at("status"), "draft") << r.body;
    EXPECT_TRUE(CALL(srv, "GET", "/api/stock").body.empty()) << "после отмены проведения";
    EXPECT_EQ(CALL(srv, "GET", "/api/stock?date=12.10.2026").status, 400);
}

/**
 * @brief Группы через API: дерево, позиция в группе, фильтры номенклатуры и остатков с подгруппами, ошибки.
 */
TEST(ApiGroups, TreeAndFilters) {
    TestServer srv;
    auto r = CALL(srv, "POST", "/api/groups", {{"name", "Кондитерка"}});
    ASSERT_EQ(r.status, 201) << r.body;
    auto const pastry = r.body.at("id");
    EXPECT_TRUE(r.body.at("parent_id").is_null());
    auto const cakes = CALL(srv, "POST", "/api/groups", {{"name", "Торты"}, {"parent_id", pastry}}).body.at("id");

    r = CALL(srv, "POST", "/api/items", {{"name", "Наполеон"}, {"kind", "product"}, {"unit", "kg"}, {"group_id", cakes}});
    ASSERT_EQ(r.status, 201) << r.body;
    EXPECT_EQ(r.body.at("group_id"), cakes);
    auto const napoleon = r.body.at("id");
    CALL(srv, "POST", "/api/items", {{"name", "Батон"}, {"kind", "product"}, {"unit", "pcs"}});

    EXPECT_EQ(CALL(srv, "GET", "/api/items?group=" + pastry.dump()).body.size(), 1U) << "Наполеон через подгруппу";
    EXPECT_EQ(CALL(srv, "GET", "/api/items").body.size(), 2U);

    auto const doc =
        CALL(srv, "POST", "/api/docs", {{"type", "production"}, {"date", "2026-10-12"}, {"lines", {{{"item_id", napoleon}, {"qty", 2500}}}}})
            .body.at("id");
    CALL(srv, "POST", "/api/docs/" + doc.dump() + "/post");
    r = CALL(srv, "GET", "/api/stock?group=" + pastry.dump());
    ASSERT_EQ(r.body.size(), 1U) << r.body;
    EXPECT_EQ(r.body.at(0).at("group_id"), cakes);

    r = CALL(srv, "PUT", "/api/groups/" + pastry.dump(), {{"name", "Кондитерка"}, {"parent_id", cakes}});
    EXPECT_EQ(r.status, 400);
    EXPECT_THAT(r.body.value("error", ""), HasSubstr("нельзя вложить в саму себя"));

    r = CALL(srv, "DELETE", "/api/groups/" + cakes.dump());
    EXPECT_EQ(r.status, 400);
    EXPECT_THAT(r.body.value("error", ""), HasSubstr("есть позиции"));

    r = CALL(srv, "PUT", "/api/items/" + napoleon.dump(), {{"group_id", nullptr}});
    EXPECT_TRUE(r.body.at("group_id").is_null()) << "убрать из группы через null: " << r.body;
    EXPECT_EQ(CALL(srv, "DELETE", "/api/groups/" + cakes.dump()).status, 200);
    EXPECT_EQ(CALL(srv, "GET", "/api/groups").body.size(), 1U);
    EXPECT_EQ(CALL(srv, "GET", "/api/stock?group=abc").status, 400);
    EXPECT_EQ(CALL(srv, "GET", "/api/stock?group=999").status, 404);
}

/**
 * @brief Цены через API: тип цен, установка цен, прайс на дату, себестоимость в поступлении.
 */
TEST(ApiPrices, TypesPriceListAndCost) {
    TestServer srv;
    auto r = CALL(srv, "GET", "/api/price-types");
    ASSERT_EQ(r.body.size(), 1U) << r.body;
    EXPECT_EQ(r.body.at(0).at("system"), true);
    r = CALL(srv, "POST", "/api/price-types", {{"name", "Вариант 1"}});
    ASSERT_EQ(r.status, 201) << r.body;
    auto const v1 = r.body.at("id");

    auto const bread = CALL(srv, "POST", "/api/items", {{"name", "Батон"}, {"kind", "product"}, {"unit", "pcs"}}).body.at("id");
    for (auto const &[type, price] : {std::pair{v1, nlohmann::json(4500)}, std::pair{nlohmann::json(1), nlohmann::json(3120)}}) {
        r = CALL(srv, "POST", "/api/docs",
                 {{"type", "prices"}, {"date", "2026-10-10"}, {"price_type_id", type}, {"lines", {{{"item_id", bread}, {"price_kop", price}}}}});
        ASSERT_EQ(r.status, 201) << r.body;
        EXPECT_EQ(CALL(srv, "POST", "/api/docs/" + r.body.at("id").dump() + "/post").status, 200);
    }

    r = CALL(srv, "GET", "/api/prices?type=" + v1.dump() + "&date=2026-10-12");
    ASSERT_EQ(r.body.size(), 1U) << r.body;
    EXPECT_EQ(r.body.at(0).at("price_kop"), 4500);
    EXPECT_EQ(r.body.at(0).at("date"), "2026-10-10");
    EXPECT_TRUE(CALL(srv, "GET", "/api/prices?type=" + v1.dump() + "&date=2026-10-09").body.empty());

    r = CALL(srv, "POST", "/api/docs", {{"type", "production"}, {"date", "2026-10-12"}, {"lines", {{{"item_id", bread}, {"qty", 1000}}}}});
    EXPECT_EQ(r.body.at("lines").at(0).at("cost_kop"), 3120) << r.body;

    EXPECT_EQ(CALL(srv, "GET", "/api/prices?date=2026-10-12").status, 400) << "без типа";
    EXPECT_EQ(CALL(srv, "GET", "/api/prices?type=99&date=2026-10-12").status, 404);
    r = CALL(srv, "DELETE", "/api/price-types/1");
    EXPECT_EQ(r.status, 400);
    EXPECT_THAT(r.body.value("error", ""), HasSubstr("встроенный"));
}

/**
 * @brief Реализация через API: клиент, цена из типа, проведение, отказ без цены, справочник клиентов.
 */
TEST(ApiSales, SaleWithPartner) {
    TestServer srv;
    auto r = CALL(srv, "POST", "/api/partners", {{"name", "Колосок"}, {"phone", "+7 900"}});
    ASSERT_EQ(r.status, 201) << r.body;
    auto const shop = r.body.at("id");
    auto const v1 = CALL(srv, "POST", "/api/price-types", {{"name", "Вариант 1"}}).body.at("id");
    auto const bread = CALL(srv, "POST", "/api/items", {{"name", "Батон"}, {"kind", "product"}, {"unit", "pcs"}}).body.at("id");
    auto const cake = CALL(srv, "POST", "/api/items", {{"name", "Торт"}, {"kind", "product"}, {"unit", "kg"}}).body.at("id");
    auto id = CALL(srv, "POST", "/api/docs",
                   {{"type", "prices"}, {"date", "2026-10-10"}, {"price_type_id", v1}, {"lines", {{{"item_id", bread}, {"price_kop", 4500}}}}})
                  .body.at("id");
    CALL(srv, "POST", "/api/docs/" + id.dump() + "/post");
    id = CALL(srv, "POST", "/api/docs",
              {{"type", "production"}, {"date", "2026-10-11"}, {"lines", {{{"item_id", bread}, {"qty", 40000}}, {{"item_id", cake}, {"qty", 5000}}}}})
             .body.at("id");
    CALL(srv, "POST", "/api/docs/" + id.dump() + "/post");

    r = CALL(srv, "POST", "/api/docs",
             {{"type", "sale"},
              {"date", "2026-10-12"},
              {"price_type_id", v1},
              {"partner_id", shop},
              {"lines", {{{"item_id", bread}, {"qty", 30000}}, {{"item_id", cake}, {"qty", 1000}}}}});
    ASSERT_EQ(r.status, 201) << r.body;
    EXPECT_EQ(r.body.at("partner_name"), "Колосок");
    EXPECT_EQ(r.body.at("lines").at(0).at("price_kop"), 4500) << r.body;
    EXPECT_TRUE(r.body.at("lines").at(1).at("price_kop").is_null()) << r.body;
    auto const sale = r.body.at("id").dump();

    r = CALL(srv, "POST", "/api/docs/" + sale + "/post");
    EXPECT_EQ(r.status, 400);
    EXPECT_THAT(r.body.value("error", ""), HasSubstr("нет цены в типе «Вариант 1» на 12.10.2026: Торт"));

    r = CALL(srv, "PUT", "/api/docs/" + sale,
             {{"date", "2026-10-12"},
              {"price_type_id", v1},
              {"partner_id", shop},
              {"lines", {{{"item_id", bread}, {"qty", 30000}}, {{"item_id", cake}, {"qty", 1000}, {"price_kop", 70000}}}}});
    ASSERT_EQ(r.status, 200) << r.body;
    EXPECT_EQ(CALL(srv, "POST", "/api/docs/" + sale + "/post").status, 200);
    r = CALL(srv, "GET", "/api/stock");
    EXPECT_EQ(r.body.at(0).at("qty"), 10000) << r.body;

    EXPECT_EQ(CALL(srv, "GET", "/api/partners").body.size(), 1U);
    r = CALL(srv, "DELETE", "/api/partners/" + shop.dump());
    EXPECT_EQ(r.status, 400);
    r = CALL(srv, "PUT", "/api/partners/" + shop.dump(), {{"archived", true}});
    EXPECT_EQ(r.body.at("archived"), true);
    EXPECT_EQ(r.body.at("phone"), "+7 900") << "незаданные поля не меняются";
    EXPECT_EQ(CALL(srv, "GET", "/api/partners").body.size(), 0U);
    EXPECT_EQ(CALL(srv, "GET", "/api/partners?archived=1").body.size(), 1U);
}

/**
 * @brief Возврат через API: что можно вернуть, создание с основанием, проведение, списание без причины.
 */
TEST(ApiReturns, ReturnableAndReturn) {
    TestServer srv;
    auto const v1 = CALL(srv, "POST", "/api/price-types", {{"name", "Вариант 1"}}).body.at("id");
    auto const bread = CALL(srv, "POST", "/api/items", {{"name", "Батон"}, {"kind", "product"}, {"unit", "pcs"}}).body.at("id");
    auto id = CALL(srv, "POST", "/api/docs", {{"type", "production"}, {"date", "2026-10-11"}, {"lines", {{{"item_id", bread}, {"qty", 40000}}}}})
                  .body.at("id");
    CALL(srv, "POST", "/api/docs/" + id.dump() + "/post");
    auto const sale =
        CALL(
            srv, "POST", "/api/docs",
            {{"type", "sale"}, {"date", "2026-10-12"}, {"price_type_id", v1}, {"lines", {{{"item_id", bread}, {"qty", 30000}, {"price_kop", 4500}}}}})
            .body.at("id");
    CALL(srv, "POST", "/api/docs/" + sale.dump() + "/post");

    auto r = CALL(srv, "GET", "/api/docs/" + sale.dump() + "/returnable");
    ASSERT_EQ(r.body.size(), 1U) << r.body;
    EXPECT_EQ(r.body.at(0).at("sold"), 30000);
    EXPECT_EQ(r.body.at(0).at("price_kop"), 4500);

    r = CALL(srv, "POST", "/api/docs",
             {{"type", "return"}, {"date", "2026-10-13"}, {"base_id", sale}, {"lines", {{{"item_id", bread}, {"qty", 5000}}}}});
    ASSERT_EQ(r.status, 201) << r.body;
    EXPECT_EQ(r.body.at("base_id"), sale);
    EXPECT_EQ(r.body.at("lines").at(0).at("price_kop"), 4500);
    EXPECT_EQ(CALL(srv, "POST", "/api/docs/" + r.body.at("id").dump() + "/post").status, 200);
    EXPECT_EQ(CALL(srv, "GET", "/api/docs/" + sale.dump() + "/returnable").body.at(0).at("returned"), 5000);
    EXPECT_EQ(CALL(srv, "GET", "/api/docs/" + id.dump() + "/returnable").status, 400) << "поступление - не реализация";

    r = CALL(srv, "POST", "/api/docs", {{"type", "writeoff"}, {"date", "2026-10-13"}, {"lines", {{{"item_id", bread}, {"qty", 1000}}}}});
    EXPECT_EQ(r.status, 400);
    EXPECT_THAT(r.body.value("error", ""), HasSubstr("укажите причину списания"));
}

/**
 * @brief Время документа в API: передаётся полем time, возвращается как ЧЧ:ММ, неверное отклоняется.
 */
TEST(ApiDocs, Time) {
    TestServer srv;
    auto const bread = CALL(srv, "POST", "/api/items", {{"name", "Батон"}, {"kind", "product"}, {"unit", "pcs"}}).body.at("id");
    auto r = CALL(srv, "POST", "/api/docs",
                  {{"type", "production"}, {"date", "2026-10-12"}, {"time", "09:15"}, {"lines", {{{"item_id", bread}, {"qty", 1000}}}}});
    ASSERT_EQ(r.status, 201) << r.body;
    EXPECT_EQ(r.body.at("time"), "09:15");
    r = CALL(srv, "POST", "/api/docs", {{"type", "production"}, {"date", "2026-10-12"}, {"lines", {{{"item_id", bread}, {"qty", 1000}}}}});
    EXPECT_EQ(r.body.at("time"), "00:00") << "без времени - начало дня";
    r = CALL(srv, "POST", "/api/docs",
             {{"type", "production"}, {"date", "2026-10-12"}, {"time", "25:00"}, {"lines", {{{"item_id", bread}, {"qty", 1000}}}}});
    EXPECT_EQ(r.status, 400);
    EXPECT_THAT(r.body.value("error", ""), HasSubstr("неверное время"));
}

/**
 * @brief Настройки и реквизиты через API.
 */
TEST(ApiSettings, Requisites) {
    TestServer srv;
    auto r = CALL(srv, "GET", "/api/settings");
    EXPECT_EQ(r.body.at("org_name"), "") << r.body;
    r = CALL(srv, "PUT", "/api/settings", {{"org_name", "ООО «Пекарня»"}, {"vat_payer", "1"}});
    ASSERT_EQ(r.status, 200) << r.body;
    EXPECT_EQ(r.body.at("vat_payer"), "1");
    r = CALL(srv, "PUT", "/api/settings", {{"org_inn", 7707083893}});
    EXPECT_EQ(r.status, 400);
    EXPECT_THAT(r.body.value("error", ""), HasSubstr("должна быть строкой"));

    r = CALL(srv, "POST", "/api/items", {{"name", "Хлеб"}, {"kind", "product"}, {"unit", "pcs"}, {"vat_rate", 10}});
    ASSERT_EQ(r.status, 201) << r.body;
    EXPECT_EQ(r.body.at("vat_rate"), 10);
    r = CALL(srv, "PUT", "/api/items/" + r.body.at("id").dump(), {{"vat_rate", nullptr}});
    EXPECT_TRUE(r.body.at("vat_rate").is_null()) << r.body;

    r = CALL(srv, "POST", "/api/partners", {{"name", "Колосок"}, {"inn", "7707083893"}, {"kpp", "773601001"}, {"address", "Москва"}});
    ASSERT_EQ(r.status, 201) << r.body;
    EXPECT_EQ(r.body.at("kpp"), "773601001");
}

/**
 * @brief Товарный отчёт через API: суммы и ошибки периода.
 */
TEST(ApiReport, Goods) {
    TestServer srv;
    auto const bread = CALL(srv, "POST", "/api/items", {{"name", "Батон"}, {"kind", "product"}, {"unit", "pcs"}}).body.at("id");
    auto id = CALL(srv, "POST", "/api/docs",
                   {{"type", "prices"}, {"date", "2026-10-01"}, {"price_type_id", 1}, {"lines", {{{"item_id", bread}, {"price_kop", 3120}}}}})
                  .body.at("id");
    CALL(srv, "POST", "/api/docs/" + id.dump() + "/post");
    id = CALL(srv, "POST", "/api/docs", {{"type", "production"}, {"date", "2026-10-10"}, {"lines", {{{"item_id", bread}, {"qty", 40000}}}}})
             .body.at("id");
    CALL(srv, "POST", "/api/docs/" + id.dump() + "/post");
    auto r = CALL(srv, "GET", "/api/report/goods?from=2026-10-10&to=2026-10-12");
    ASSERT_EQ(r.status, 200) << r.body;
    EXPECT_EQ(r.body.at("in_sum"), 124800);
    EXPECT_EQ(r.body.at("closing_sum"), 124800);
    EXPECT_EQ(r.body.at("incoming").at(0).at("type"), "production");
    EXPECT_EQ(r.body.at("items").at(0).at("closing"), 40000);
    r = CALL(srv, "GET", "/api/report/goods?from=2026-10-10&to=2026-10-12&price_type=1");
    EXPECT_EQ(r.body.at("closing_price_sum"), 124800) << "по типу «Себестоимость» на момент: " << r.body;
    EXPECT_EQ(CALL(srv, "GET", "/api/report/goods?from=2026-10-10&to=2026-10-12&price_type=99").status, 404);
    EXPECT_EQ(CALL(srv, "GET", "/api/report/goods?from=2026-10-12&to=2026-10-10").status, 400);
    EXPECT_EQ(CALL(srv, "GET", "/api/report/goods?from=x&to=2026-10-10").status, 400);
}

/**
 * @brief Инвентаризация через API: учётные остатки на момент без документа, область, проведение.
 */
TEST(ApiInventory, FillAndPost) {
    TestServer srv;
    auto const bread = CALL(srv, "POST", "/api/items", {{"name", "Батон"}, {"kind", "product"}, {"unit", "pcs"}}).body.at("id");
    auto const cake = CALL(srv, "POST", "/api/items", {{"name", "Торт"}, {"kind", "product"}, {"unit", "kg"}}).body.at("id");
    auto id = CALL(srv, "POST", "/api/docs",
                   {{"type", "production"},
                    {"date", "2026-10-10"},
                    {"time", "08:00"},
                    {"lines", {{{"item_id", bread}, {"qty", 10000}}, {{"item_id", cake}, {"qty", 5000}}}}})
                  .body.at("id");
    CALL(srv, "POST", "/api/docs/" + id.dump() + "/post");

    auto r = CALL(srv, "GET", "/api/stock?date=2026-10-10&time=07:00");
    EXPECT_TRUE(r.body.empty()) << "до поступления в 08:00: " << r.body;
    r = CALL(srv, "GET", "/api/stock?date=2026-10-10&time=09:00");
    EXPECT_EQ(r.body.size(), 2U) << r.body;

    r = CALL(srv, "POST", "/api/docs",
             {{"type", "inventory"}, {"date", "2026-10-10"}, {"time", "20:00"}, {"lines", {{{"item_id", bread}, {"qty", 9000}}}}});
    ASSERT_EQ(r.status, 201) << r.body;
    auto const inv = r.body.at("id").dump();
    EXPECT_TRUE(r.body.at("group_id").is_null());
    EXPECT_EQ(CALL(srv, "POST", "/api/docs/" + inv + "/post").status, 200);
    r = CALL(srv, "GET", "/api/stock");
    ASSERT_EQ(r.body.size(), 1U) << "торт обнулён: " << r.body;
    EXPECT_EQ(r.body.at(0).at("qty"), 9000);
    r = CALL(srv, "GET", "/api/stock?date=2026-10-10&time=21:00&exclude=" + inv);
    EXPECT_EQ(r.body.size(), 2U) << "учёт без движений инвентаризации: " << r.body;
}

/**
 * @brief Отметка о перепроведении и пошаговое перепроведение через API: поступление после
 * инвентаризации исправлено задним числом, /api/repost показывает причину, шаги идут до конца.
 */
TEST(ApiRepost, MarkAndSteps) {
    TestServer srv;
    auto const bread = CALL(srv, "POST", "/api/items", {{"name", "Батон"}, {"kind", "product"}, {"unit", "pcs"}}).body.at("id");
    auto const prod = CALL(srv, "POST", "/api/docs",
                           {{"type", "production"}, {"date", "2026-10-10"}, {"time", "08:00"}, {"lines", {{{"item_id", bread}, {"qty", 10000}}}}})
                          .body.at("id")
                          .dump();
    CALL(srv, "POST", "/api/docs/" + prod + "/post");
    auto const inv = CALL(srv, "POST", "/api/docs",
                          {{"type", "inventory"}, {"date", "2026-10-10"}, {"time", "20:00"}, {"lines", {{{"item_id", bread}, {"qty", 9000}}}}})
                         .body.at("id")
                         .dump();
    CALL(srv, "POST", "/api/docs/" + inv + "/post");

    auto r = CALL(srv, "GET", "/api/repost");
    EXPECT_EQ(r.body.at("needed"), false) << r.body;

    CALL(srv, "PUT", "/api/docs/" + prod, {{"date", "2026-10-10"}, {"time", "08:00"}, {"lines", {{{"item_id", bread}, {"qty", 12000}}}}});
    r = CALL(srv, "GET", "/api/repost");
    ASSERT_EQ(r.body.at("needed"), true) << r.body;
    EXPECT_EQ(r.body.at("from").at("moment"), "2026-10-10 08:00:00");
    EXPECT_EQ(r.body.at("cause").at("title"), "Поступление из производства № 1 от 10.10.2026 08:00");
    EXPECT_EQ(r.body.at("count"), 2);
    EXPECT_EQ(CALL(srv, "GET", "/api/repost/count?date=2026-10-10&time=12:00").body.at("count"), 1) << "только инвентаризация";
    EXPECT_TRUE(r.body.at("run").is_null()) << "прогона ещё нет: " << r.body;

    std::vector<std::string> titles;
    for (int i = 0; i < 5; ++i) {
        r = CALL(srv, "POST", "/api/repost/step", {{"date", "2026-10-10"}, {"time", "00:00"}});
        ASSERT_EQ(r.status, 200) << r.body;
        ASSERT_TRUE(r.body.at("error").is_null()) << r.body;
        if (r.body.at("doc").is_null()) {
            break;
        }
        titles.push_back(r.body.at("doc").at("title"));
        if (i == 0) {
            // как после перезагрузки страницы: прогон виден в состоянии
            auto const state = CALL(srv, "GET", "/api/repost");
            ASSERT_FALSE(state.body.at("run").is_null()) << state.body;
            EXPECT_EQ(state.body.at("run").at("from"), "2026-10-10 00:00:00");
            EXPECT_EQ(state.body.at("run").at("done"), 1);
            EXPECT_EQ(state.body.at("run").at("remaining"), 1) << "осталась инвентаризация";
            EXPECT_EQ(state.body.at("run").at("after").at("title"), "Поступление из производства № 1 от 10.10.2026 08:00");
        }
    }
    EXPECT_THAT(titles, testing::ElementsAre("Поступление из производства № 1 от 10.10.2026 08:00", "Инвентаризация № 1 от 10.10.2026 20:00"));
    EXPECT_EQ(r.body.at("needed"), false) << "дошли до конца: " << r.body;
    EXPECT_EQ(r.body.at("done"), 2) << r.body;
    EXPECT_TRUE(CALL(srv, "GET", "/api/repost").body.at("run").is_null()) << "прогон завершён";
    r = CALL(srv, "GET", "/api/stock");
    EXPECT_EQ(r.body.at(0).at("qty"), 9000) << "после инвентаризации насчитанное: " << r.body;

    EXPECT_EQ(CALL(srv, "POST", "/api/repost/step", {{"date", "10.10.2026"}}).status, 400) << "дата не в формате ГГГГ-ММ-ДД";
}

/**
 * @brief Закрытие периода через API: дата читается и меняется, документ в закрытом периоде не создаётся.
 */
TEST(ApiPeriod, CloseAndReject) {
    TestServer srv;
    auto r = CALL(srv, "GET", "/api/period");
    EXPECT_TRUE(r.body.at("closed_till").is_null()) << r.body;

    r = CALL(srv, "PUT", "/api/period", {{"closed_till", "2026-09-30"}});
    ASSERT_EQ(r.status, 200) << r.body;
    EXPECT_EQ(r.body.at("closed_till"), "2026-09-30");
    EXPECT_EQ(CALL(srv, "GET", "/api/period").body.at("closed_till"), "2026-09-30");
    EXPECT_FALSE(CALL(srv, "GET", "/api/settings").body.contains("closed_till")) << "дата закрытия не реквизит";

    auto const bread = CALL(srv, "POST", "/api/items", {{"name", "Батон"}, {"kind", "product"}, {"unit", "pcs"}}).body.at("id");
    r = CALL(srv, "POST", "/api/docs", {{"type", "production"}, {"date", "2026-09-30"}, {"lines", {{{"item_id", bread}, {"qty", 1000}}}}});
    EXPECT_EQ(r.status, 400) << r.body;
    EXPECT_THAT(r.body.value("error", ""), testing::HasSubstr("период по 30.09.2026 закрыт"));
    r = CALL(srv, "POST", "/api/docs", {{"type", "production"}, {"date", "2026-10-01"}, {"lines", {{{"item_id", bread}, {"qty", 1000}}}}});
    EXPECT_EQ(r.status, 201) << "после даты закрытия документ создаётся: " << r.body;

    r = CALL(srv, "PUT", "/api/period", {{"closed_till", "30.09.2026"}});
    EXPECT_EQ(r.status, 400) << "дата не в формате ГГГГ-ММ-ДД: " << r.body;
    r = CALL(srv, "PUT", "/api/period", {{"closed_till", 5}});
    EXPECT_EQ(r.status, 400) << r.body;
    r = CALL(srv, "PUT", "/api/period", {{"closed_till", nullptr}});
    ASSERT_EQ(r.status, 200) << r.body;
    EXPECT_TRUE(r.body.at("closed_till").is_null()) << "null открывает все периоды: " << r.body;
}

/**
 * @brief Фото позиции: загрузка телом запроса, отдача с типом и ETag, 304 по If-None-Match, отказ не картинке,
 * удаление; запрос больше предела отклоняется 413 с текстом.
 */
TEST(ApiCatalog, Photo) {
    TestServer srv;
    auto const id = CALL(srv, "POST", "/api/items", {{"name", "Батон"}, {"kind", "product"}, {"unit", "pcs"}}).body.at("id").get<std::int64_t>();
    auto const path = "/api/items/" + std::to_string(id) + "/photo";
    EXPECT_TRUE(CALL(srv, "GET", "/api/items/" + std::to_string(id)).body.at("photo").is_null()) << "фото ещё нет";
    EXPECT_EQ(CALL(srv, "GET", path).status, 404) << "GET фото, которого нет";

    auto cli = srv.client();
    std::string const png = std::string("\x89PNG\r\n\x1A\n\0\0\0\rIHDR", 16);
    auto put = cli.Put(path, png, "image/png");
    ASSERT_TRUE(put) << httplib::to_string(put.error());
    ASSERT_EQ(put->status, 200) << put->body;
    auto const etag = nlohmann::json::parse(put->body).at("photo");
    ASSERT_TRUE(etag.is_string()) << "ответ PUT - позиция с etag фото: " << put->body;

    auto got = cli.Get(path);
    ASSERT_TRUE(got);
    EXPECT_EQ(got->status, 200);
    EXPECT_EQ(got->get_header_value("Content-Type"), "image/png");
    EXPECT_EQ(got->body, png) << "фото должно вернуться байт в байт";
    EXPECT_EQ(got->get_header_value("ETag"), "\"" + etag.get<std::string>() + "\"");

    auto cached = cli.Get(path, {{"If-None-Match", got->get_header_value("ETag")}});
    ASSERT_TRUE(cached);
    EXPECT_EQ(cached->status, 304) << "браузер с актуальным фото получает 304";

    put = cli.Put(path, "<svg/>", "image/svg+xml");
    ASSERT_TRUE(put);
    EXPECT_EQ(put->status, 400) << put->body;
    EXPECT_THAT(put->body, HasSubstr("JPEG, PNG или WebP"));

    put = cli.Put(path, std::string(11 * 1024 * 1024, 'x'), "image/jpeg");
    ASSERT_TRUE(put) << httplib::to_string(put.error());
    EXPECT_EQ(put->status, 413) << "больше предела запроса";
    EXPECT_THAT(put->body, HasSubstr("запрос больше 10 МБ"));

    auto const removed = CALL(srv, "DELETE", path);
    EXPECT_EQ(removed.status, 200) << removed.body;
    EXPECT_TRUE(removed.body.at("photo").is_null()) << removed.body;
    EXPECT_EQ(CALL(srv, "GET", path).status, 404) << "после удаления фото нет";
}

namespace {

/// Запрос клиентом cli с телом JSON; код ответа или -1 без ответа.
int status_of(httplib::Client &cli, std::string const &method, std::string const &path, nlohmann::json const &body = nlohmann::json::object()) {
    auto const text = body.dump();
    httplib::Result res = method == "GET"    ? cli.Get(path)
                          : method == "POST" ? cli.Post(path, text, "application/json")
                          : method == "PUT"  ? cli.Put(path, text, "application/json")
                                             : cli.Delete(path);
    return res ? res->status : -1;
}

/// Вход через API: cookie сессии или пустая строка.
std::string login_as(TestServer const &srv, std::string const &login, std::string const &password) {
    auto cli = srv.anonymous();
    auto const res = cli.Post("/api/auth/login", nlohmann::json{{"login", login}, {"password", password}}.dump(), "application/json");
    return res && res->status == 200 ? TestServer::cookie_of(*res) : std::string{};
}

} // namespace

/**
 * @brief Без входа API отвечает 401 (кроме версии и входа); setup второй раз отклоняется; после выхода сессия
 * не действует.
 */
TEST(ApiAuth, LoginRequired) {
    TestServer srv;
    auto anon = srv.anonymous();
    EXPECT_EQ(status_of(anon, "GET", "/api/version"), 200) << "версия доступна без входа";
    EXPECT_EQ(status_of(anon, "GET", "/api/items"), 401) << "справочник без входа";
    EXPECT_EQ(status_of(anon, "POST", "/api/docs", {{"type", "order"}}), 401);
    auto const state = anon.Get("/api/auth");
    ASSERT_TRUE(state);
    auto const body = nlohmann::json::parse(state->body);
    EXPECT_EQ(body.at("setup"), false) << "администратор уже создан: " << state->body;
    EXPECT_TRUE(body.at("user").is_null()) << state->body;
    EXPECT_EQ(status_of(anon, "POST", "/api/auth/setup", {{"login", "evil"}, {"password", "secret1"}}), 400) << "повторный setup";

    EXPECT_EQ(login_as(srv, "admin", "wrong!"), "") << "неверный пароль";
    auto const cookie = login_as(srv, "admin", "secret1");
    ASSERT_FALSE(cookie.empty());
    auto cli = srv.client_as(cookie);
    auto const me = cli.Get("/api/auth");
    ASSERT_TRUE(me);
    EXPECT_EQ(nlohmann::json::parse(me->body).at("user").at("login"), "admin") << me->body;
    auto const out = cli.Post("/api/auth/logout", "{}", "application/json");
    ASSERT_TRUE(out);
    EXPECT_THAT(out->get_header_value("Set-Cookie"), HasSubstr("Max-Age=0")) << "выход стирает cookie";
    EXPECT_EQ(status_of(cli, "GET", "/api/items"), 401) << "после выхода сессия закрыта";
}

/**
 * @brief Права ролей: продавец ведёт документы и клиентов, установки цен не видит, справочники только читает;
 * технолог всё, кроме пользователей; пользователями управляет администратор.
 */
TEST(ApiAuth, Roles) {
    TestServer srv;
    ASSERT_EQ(CALL(srv, "POST", "/api/users", {{"login", "olga"}, {"role", "seller"}, {"password", "secret1"}}).status, 201);
    ASSERT_EQ(CALL(srv, "POST", "/api/users", {{"login", "tanya"}, {"role", "technologist"}, {"password", "secret1"}}).status, 201);
    auto const bread = CALL(srv, "POST", "/api/items", {{"name", "Батон"}, {"kind", "product"}, {"unit", "pcs"}}).body.at("id").get<std::int64_t>();
    auto const prices =
        CALL(srv, "POST", "/api/docs",
             {{"type", "prices"}, {"date", "2026-10-01"}, {"price_type_id", 1}, {"lines", {{{"item_id", bread}, {"price_kop", 3000}}}}})
            .body.at("id")
            .get<std::int64_t>();

    auto seller = srv.client_as(login_as(srv, "olga", "secret1"));
    EXPECT_EQ(status_of(seller, "GET", "/api/items"), 200) << "продавец видит номенклатуру";
    EXPECT_EQ(status_of(seller, "GET", "/api/stock"), 200) << "и остатки";
    EXPECT_EQ(status_of(seller, "POST", "/api/items", {{"name", "Багет"}, {"kind", "product"}, {"unit", "pcs"}}), 403) << "но не меняет";
    EXPECT_EQ(status_of(seller, "PUT", "/api/settings", {{"org_name", "x"}}), 403);
    EXPECT_EQ(status_of(seller, "PUT", "/api/period", {{"closed_till", nullptr}}), 403);
    EXPECT_EQ(status_of(seller, "POST", "/api/price-types", {{"name", "Опт"}}), 403);
    EXPECT_EQ(status_of(seller, "POST", "/api/repost/step", {{"date", "2026-10-01"}}), 403);
    EXPECT_EQ(status_of(seller, "POST", "/api/docs",
                        {{"type", "prices"}, {"date", "2026-10-01"}, {"price_type_id", 1}, {"lines", {{{"item_id", bread}, {"price_kop", 1}}}}}),
              403)
        << "установка цен не продавцу";
    EXPECT_EQ(status_of(seller, "POST", "/api/docs/" + std::to_string(prices) + "/post"), 403) << "провести установку цен";
    EXPECT_EQ(status_of(seller, "GET", "/api/docs/" + std::to_string(prices)), 403) << "установку цен продавец не открывает";
    auto const listed = seller.Get("/api/docs");
    ASSERT_TRUE(listed);
    EXPECT_EQ(nlohmann::json::parse(listed->body).size(), 0U) << "установки цен нет в списке продавца: " << listed->body;
    EXPECT_EQ(status_of(seller, "GET", "/api/prices?type=1&date=2026-10-01"), 200) << "цены для строк реализации продавцу нужны";
    EXPECT_EQ(
        status_of(seller, "POST", "/api/docs", {{"type", "production"}, {"date", "2026-10-02"}, {"lines", {{{"item_id", bread}, {"qty", 1000}}}}}),
        201)
        << "поступление продавцу можно";
    EXPECT_EQ(status_of(seller, "POST", "/api/partners", {{"name", "Колосок"}}), 201) << "клиентов продавец ведёт";
    EXPECT_EQ(status_of(seller, "GET", "/api/users"), 403);

    auto tech = srv.client_as(login_as(srv, "tanya", "secret1"));
    EXPECT_EQ(status_of(tech, "POST", "/api/items", {{"name", "Багет"}, {"kind", "product"}, {"unit", "pcs"}}), 201);
    EXPECT_EQ(status_of(tech, "POST", "/api/docs/" + std::to_string(prices) + "/post"), 200) << "технолог проводит установку цен";
    auto const tech_list = tech.Get("/api/docs");
    ASSERT_TRUE(tech_list);
    EXPECT_EQ(nlohmann::json::parse(tech_list->body).size(), 2U) << "технолог видит и установку цен: " << tech_list->body;
    EXPECT_EQ(status_of(tech, "GET", "/api/users"), 403) << "пользователи только администратору";
    EXPECT_EQ(status_of(tech, "POST", "/api/users", {{"login", "x"}, {"role", "admin"}, {"password", "secret1"}}), 403);
}

/**
 * @brief Администратор меняет роль, пароль и блокирует; блокировка сразу закрывает сессию; свой пароль меняется
 * со старым паролем и выдаёт новую сессию.
 */
TEST(ApiAuth, ManageUsers) {
    TestServer srv;
    auto const id = CALL(srv, "POST", "/api/users", {{"login", "olga"}, {"name", "Ольга"}, {"role", "seller"}, {"password", "secret1"}})
                        .body.at("id")
                        .get<std::int64_t>();
    auto const path = "/api/users/" + std::to_string(id);
    auto olga = srv.client_as(login_as(srv, "olga", "secret1"));

    auto const list = CALL(srv, "GET", "/api/users");
    ASSERT_EQ(list.body.size(), 2U) << list.body;
    EXPECT_FALSE(list.body.dump().find("pass") != std::string::npos) << "хеш пароля не отдаётся: " << list.body;

    auto r = CALL(srv, "PUT", path, {{"role", "technologist"}});
    EXPECT_EQ(r.body.at("role"), "technologist") << r.body;
    EXPECT_EQ(status_of(olga, "POST", "/api/price-types", {{"name", "Опт"}}), 201) << "новая роль действует сразу";

    r = CALL(srv, "PUT", path, {{"blocked", true}});
    EXPECT_EQ(r.body.at("blocked"), true) << r.body;
    EXPECT_EQ(status_of(olga, "GET", "/api/items"), 401) << "заблокированный выкинут";
    EXPECT_EQ(login_as(srv, "olga", "secret1"), "") << "и не может войти";

    CALL(srv, "PUT", path, {{"blocked", false}, {"password", "newpass"}});
    EXPECT_EQ(login_as(srv, "olga", "secret1"), "") << "старый пароль после смены администратором";
    olga = srv.client_as(login_as(srv, "olga", "newpass"));
    EXPECT_EQ(status_of(olga, "PUT", "/api/auth/password", {{"old_password", "wrong!"}, {"password", "mypass1"}}), 400) << "неверный текущий пароль";
    auto const changed =
        olga.Put("/api/auth/password", nlohmann::json{{"old_password", "newpass"}, {"password", "mypass1"}}.dump(), "application/json");
    ASSERT_TRUE(changed);
    EXPECT_EQ(changed->status, 200) << changed->body;
    auto fresh = srv.client_as(TestServer::cookie_of(*changed));
    EXPECT_EQ(status_of(fresh, "GET", "/api/items"), 200) << "после смены своего пароля выдаётся новая сессия";
    EXPECT_EQ(status_of(olga, "GET", "/api/items"), 401) << "старая сессия закрыта";

    auto const self = CALL(srv, "GET", "/api/auth").body.at("user").at("id").get<std::int64_t>();
    r = CALL(srv, "PUT", "/api/users/" + std::to_string(self), {{"role", "seller"}});
    EXPECT_EQ(r.status, 400) << "последнего администратора не понизить: " << r.body;
}

/**
 * @brief Документ, созданный через API, показывает автора и того, кто его провёл.
 */
TEST(ApiAuth, DocumentAuthor) {
    TestServer srv;
    CALL(srv, "POST", "/api/users", {{"login", "olga"}, {"name", "Ольга"}, {"role", "seller"}, {"password", "secret1"}});
    auto const bread = CALL(srv, "POST", "/api/items", {{"name", "Батон"}, {"kind", "product"}, {"unit", "pcs"}}).body.at("id");
    auto const doc =
        CALL(srv, "POST", "/api/docs", {{"type", "production"}, {"date", "2026-10-02"}, {"lines", {{{"item_id", bread}, {"qty", 1000}}}}});
    EXPECT_EQ(doc.body.at("created_by"), "Админ") << doc.body;
    auto olga = srv.client_as(login_as(srv, "olga", "secret1"));
    auto const posted = olga.Post("/api/docs/" + std::to_string(doc.body.at("id").get<std::int64_t>()) + "/post", "{}", "application/json");
    ASSERT_TRUE(posted);
    auto const body = nlohmann::json::parse(posted->body);
    EXPECT_EQ(body.at("created_by"), "Админ") << posted->body;
    EXPECT_EQ(body.at("updated_by"), "Ольга") << posted->body;
}

/**
 * @brief Резервные копии только администратору: копия сейчас, скачивание файла SQLite, расписание, удаление.
 */
TEST(ApiBackups, AdminOnly) {
    TestServer srv;
    CALL(srv, "POST", "/api/users", {{"login", "tanya"}, {"role", "technologist"}, {"password", "secret1"}});
    auto tech = srv.client_as(login_as(srv, "tanya", "secret1"));
    EXPECT_EQ(status_of(tech, "GET", "/api/backups"), 403) << "технологу копии недоступны";
    EXPECT_EQ(status_of(tech, "POST", "/api/backups"), 403);

    auto const created = CALL(srv, "POST", "/api/backups");
    ASSERT_EQ(created.status, 201) << created.body;
    auto const name = created.body.at("created").get<std::string>();
    EXPECT_THAT(name, testing::MatchesRegex("bakery-[0-9]{8}-[0-9]{6}-manual\\.db"));
    ASSERT_EQ(created.body.at("backups").size(), 1U) << created.body;
    EXPECT_FALSE(created.body.at("schedule").at("enabled").get<bool>());

    auto cli = srv.client();
    auto const file = cli.Get("/api/backups/" + name);
    ASSERT_TRUE(file);
    EXPECT_EQ(file->status, 200);
    EXPECT_THAT(file->body, testing::StartsWith(std::string("SQLite format 3\0", 16))) << "скачивается файл базы";
    EXPECT_THAT(file->get_header_value("Content-Disposition"), HasSubstr(name));
    EXPECT_EQ(status_of(tech, "GET", "/api/backups/" + name), 403) << "скачать копию технологу нельзя";
    EXPECT_EQ(status_of(cli, "GET", "/api/backups/..%2Fbakery.db"), 404) << "выход из каталога копий";

    auto const schedule = CALL(srv, "PUT", "/api/backups/schedule", {{"enabled", true}, {"time", "02:30"}, {"keep", 7}});
    ASSERT_EQ(schedule.status, 200) << schedule.body;
    EXPECT_EQ(schedule.body.at("schedule").at("time"), "02:30");
    EXPECT_EQ(schedule.body.at("schedule").at("keep"), 7);
    EXPECT_EQ(CALL(srv, "PUT", "/api/backups/schedule", {{"keep", 1000}}).status, 400);
    EXPECT_FALSE(CALL(srv, "GET", "/api/settings").body.contains("backup_time")) << "расписание не среди реквизитов";

    auto const removed = CALL(srv, "DELETE", "/api/backups/" + name);
    EXPECT_EQ(removed.status, 200) << removed.body;
    EXPECT_EQ(removed.body.at("backups").size(), 0U);
}
