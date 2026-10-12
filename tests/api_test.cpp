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
    auto const epoch = r.body.at("epoch");
    EXPECT_EQ(CALL(srv, "GET", "/api/repost/count?date=2026-10-10&time=12:00").body.at("count"), 1) << "только инвентаризация";

    nlohmann::json after = nullptr;
    std::vector<std::string> titles;
    for (int i = 0; i < 5; ++i) {
        r = CALL(srv, "POST", "/api/repost/step", {{"date", "2026-10-10"}, {"time", "00:00"}, {"after", after}, {"epoch", epoch}});
        ASSERT_EQ(r.status, 200) << r.body;
        ASSERT_TRUE(r.body.at("error").is_null()) << r.body;
        if (r.body.at("doc").is_null()) {
            break;
        }
        titles.push_back(r.body.at("doc").at("title"));
        after = {{"moment", r.body.at("doc").at("moment")}, {"id", r.body.at("doc").at("id")}};
    }
    EXPECT_THAT(titles, testing::ElementsAre("Поступление из производства № 1 от 10.10.2026 08:00", "Инвентаризация № 1 от 10.10.2026 20:00"));
    EXPECT_EQ(r.body.at("needed"), false) << "дошли до конца: " << r.body;
    r = CALL(srv, "GET", "/api/stock");
    EXPECT_EQ(r.body.at(0).at("qty"), 9000) << "после инвентаризации насчитанное: " << r.body;

    EXPECT_EQ(CALL(srv, "POST", "/api/repost/step", {{"date", "10.10.2026"}}).status, 400) << "дата не в формате ГГГГ-ММ-ДД";
}
