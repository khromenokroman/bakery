#include "api.hpp"

#include <httplib.h>

#include <algorithm>
#include <charconv>
#include <exception>
#include <nlohmann/json.hpp>
#include <stdexcept>

#include "errors.hpp"
#include "json_io.hpp"
#include "requirements.hpp"
#include "schema.hpp"

namespace {

constexpr char const JSON_TYPE[] = "application/json; charset=utf-8";

void send_json(httplib::Response &res, nlohmann::json const &body, int status = 200) {
    res.status = status;
    res.set_content(body.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace), JSON_TYPE);
}

void send_error(httplib::Response &res, int status, std::string const &message) { send_json(res, {{"error", message}}, status); }

/// Разбирает список id через запятую: "1,2,3".
std::vector<std::int64_t> parse_ids(std::string const &text) {
    std::vector<std::int64_t> ids;
    std::size_t pos = 0;
    while (pos <= text.size() && !text.empty()) {
        auto const end = std::min(text.find(',', pos), text.size());
        std::int64_t id = 0;
        auto const [ptr, ec] = std::from_chars(text.data() + pos, text.data() + end, id);
        if (ec != std::errc{} || ptr != text.data() + end) {
            throw UserError("неверный список id: " + text);
        }
        ids.push_back(id);
        pos = end + 1;
    }
    return ids;
}

/// id из параметра запроса.
std::int64_t query_id(httplib::Request const &req, std::string const &key) {
    auto const text = req.get_param_value(key);
    std::int64_t id = 0;
    auto const [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), id);
    if (ec != std::errc{} || ptr != text.data() + text.size()) {
        throw UserError("параметр " + key + " должен быть числом: " + text);
    }
    return id;
}

/// id из пути запроса; не число означает несуществующий объект.
std::int64_t path_id(httplib::Request const &req) {
    auto const &text = req.path_params.at("id");
    std::int64_t id = 0;
    auto const [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), id);
    if (ec != std::errc{} || ptr != text.data() + text.size()) {
        throw NotFoundError("неверный id: " + text);
    }
    return id;
}

} // namespace

Api::Api(Database &db, std::string web_dir) : m_web_dir{std::move(web_dir)}, m_catalog{db}, m_groups{db}, m_docs{db}, m_stock{db}, m_db{db} {}

Api::Handler Api::guarded(Handler handler) {
    return [this, handler = std::move(handler)](httplib::Request const &req, httplib::Response &res) {
        auto const lock = m_db.lock();
        try {
            handler(req, res);
        } catch (UserError const &e) {
            send_error(res, 400, e.what());
        } catch (NotFoundError const &e) {
            send_error(res, 404, e.what());
        }
    };
}

void Api::register_routes(httplib::Server &server) {
    server.Get("/api/version", guarded([this](httplib::Request const &, httplib::Response &res) {
                   send_json(res, {{"version", BAKERY_VERSION}, {"schema", current_schema_version(m_db)}});
               }));
    register_catalog(server);
    register_docs(server);
    register_groups(server);

    if (!m_web_dir.empty() && !server.set_mount_point("/", m_web_dir)) {
        throw std::runtime_error("каталог веб-интерфейса не найден: " + m_web_dir);
    }

    server.set_exception_handler([](httplib::Request const &, httplib::Response &res, std::exception_ptr ep) {
        try {
            std::rethrow_exception(ep);
        } catch (std::exception const &e) {
            send_error(res, 500, e.what());
        } catch (...) {
            send_error(res, 500, "неизвестная ошибка");
        }
    });

    server.set_error_handler([](httplib::Request const &req, httplib::Response &res) {
        if (req.path.starts_with("/api/") && res.body.empty()) {
            send_error(res, res.status, res.status == 404 ? "нет такого метода: " + req.method + " " + req.path : "ошибка запроса");
        }
    });
}

void Api::register_catalog(httplib::Server &server) {
    server.Get("/api/items", guarded([this](httplib::Request const &req, httplib::Response &res) {
                   ItemFilter filter;
                   if (req.has_param("kind")) {
                       filter.kind = kind_from_string(req.get_param_value("kind"));
                   }
                   filter.with_archived = req.get_param_value("archived") == "1";
                   if (req.has_param("group")) {
                       filter.group = query_id(req, "group");
                   }
                   auto out = nlohmann::json::array();
                   for (auto const &item : m_catalog.list(filter)) {
                       out.push_back(to_json(item));
                   }
                   send_json(res, out);
               }));

    server.Post("/api/items", guarded([this](httplib::Request const &req, httplib::Response &res) {
                    auto const j = parse_object(req.body);
                    Item item;
                    item.name = get_string(j, "name");
                    item.kind = kind_from_string(get_string(j, "kind"));
                    item.unit = unit_from_string(get_string(j, "unit"));
                    item.group_id = get_optional_id(j, "group_id", std::nullopt);
                    auto const id = m_catalog.create(item);
                    send_json(res, to_json(m_catalog.get(id)), 201);
                }));

    server.Get("/api/items/:id",
               guarded([this](httplib::Request const &req, httplib::Response &res) { send_json(res, to_json(m_catalog.get(path_id(req)))); }));

    server.Put("/api/items/:id", guarded([this](httplib::Request const &req, httplib::Response &res) {
                   auto const id = path_id(req);
                   auto const item = item_from_json(parse_object(req.body), m_catalog.get(id));
                   m_catalog.update(item);
                   send_json(res, to_json(m_catalog.get(id)));
               }));

    server.Delete("/api/items/:id", guarded([this](httplib::Request const &req, httplib::Response &res) {
                      m_catalog.remove(path_id(req));
                      send_json(res, nlohmann::json::object());
                  }));

    server.Get("/api/items/:id/recipe", guarded([this](httplib::Request const &req, httplib::Response &res) {
                   auto const id = path_id(req);
                   (void)m_catalog.get(id);
                   auto const recipe = m_catalog.recipe(id);
                   send_json(res, recipe ? to_json(*recipe) : nlohmann::json(nullptr));
               }));

    server.Put("/api/items/:id/recipe", guarded([this](httplib::Request const &req, httplib::Response &res) {
                   auto const id = path_id(req);
                   m_catalog.set_recipe(id, recipe_from_json(parse_object(req.body)));
                   send_json(res, to_json(*m_catalog.recipe(id)));
               }));

    server.Delete("/api/items/:id/recipe", guarded([this](httplib::Request const &req, httplib::Response &res) {
                      auto const id = path_id(req);
                      (void)m_catalog.get(id);
                      m_catalog.remove_recipe(id);
                      send_json(res, nlohmann::json::object());
                  }));
}

void Api::register_docs(httplib::Server &server) {
    server.Get("/api/docs", guarded([this](httplib::Request const &req, httplib::Response &res) {
                   DocFilter filter;
                   if (req.has_param("type")) {
                       auto const text = req.get_param_value("type");
                       for (std::size_t pos = 0; pos <= text.size();) {
                           auto const end = std::min(text.find(',', pos), text.size());
                           filter.types.push_back(doc_type_from_string(text.substr(pos, end - pos)));
                           pos = end + 1;
                       }
                   }
                   if (req.has_param("from")) {
                       check_date(req.get_param_value("from"));
                       filter.date_from = req.get_param_value("from");
                   }
                   if (req.has_param("to")) {
                       check_date(req.get_param_value("to"));
                       filter.date_to = req.get_param_value("to");
                   }
                   auto out = nlohmann::json::array();
                   for (auto const &doc : m_docs.list(filter)) {
                       out.push_back(to_json(doc));
                   }
                   send_json(res, out);
               }));

    server.Post("/api/docs", guarded([this](httplib::Request const &req, httplib::Response &res) {
                    auto const j = parse_object(req.body);
                    if (!j.contains("type")) {
                        throw UserError("нет поля type");
                    }
                    auto const id = m_docs.create(document_from_json(j));
                    send_json(res, to_json(m_docs.get(id)), 201);
                }));

    server.Get("/api/docs/:id",
               guarded([this](httplib::Request const &req, httplib::Response &res) { send_json(res, to_json(m_docs.get(path_id(req)))); }));

    server.Put("/api/docs/:id", guarded([this](httplib::Request const &req, httplib::Response &res) {
                   auto doc = document_from_json(parse_object(req.body));
                   doc.id = path_id(req);
                   m_docs.update(doc);
                   send_json(res, to_json(m_docs.get(doc.id)));
               }));

    server.Delete("/api/docs/:id", guarded([this](httplib::Request const &req, httplib::Response &res) {
                      m_docs.remove(path_id(req));
                      send_json(res, nlohmann::json::object());
                  }));

    server.Post("/api/docs/:id/post", guarded([this](httplib::Request const &req, httplib::Response &res) {
                    auto const id = path_id(req);
                    m_docs.post(id);
                    send_json(res, to_json(m_docs.get(id)));
                }));

    server.Post("/api/docs/:id/unpost", guarded([this](httplib::Request const &req, httplib::Response &res) {
                    auto const id = path_id(req);
                    m_docs.unpost(id);
                    send_json(res, to_json(m_docs.get(id)));
                }));

    server.Get("/api/stock", guarded([this](httplib::Request const &req, httplib::Response &res) {
                   std::optional<std::string> date;
                   if (req.has_param("date")) {
                       check_date(req.get_param_value("date"));
                       date = req.get_param_value("date");
                   }
                   auto out = nlohmann::json::array();
                   std::optional<std::int64_t> group;
                   if (req.has_param("group")) {
                       group = query_id(req, "group");
                       (void)m_groups.get(*group);
                   }
                   for (auto const &b : m_stock.balances(date, group)) {
                       out.push_back(to_json(b));
                   }
                   send_json(res, out);
               }));

    server.Get("/api/requirements", guarded([this](httplib::Request const &req, httplib::Response &res) {
                   send_json(res, to_json(compute_requirements(m_db, parse_ids(req.get_param_value("docs")))));
               }));
}

void Api::register_groups(httplib::Server &server) {
    server.Get("/api/groups", guarded([this](httplib::Request const &, httplib::Response &res) {
                   auto out = nlohmann::json::array();
                   for (auto const &g : m_groups.list()) {
                       out.push_back(to_json(g));
                   }
                   send_json(res, out);
               }));

    server.Post("/api/groups", guarded([this](httplib::Request const &req, httplib::Response &res) {
                    auto const id = m_groups.create(group_from_json(parse_object(req.body)));
                    send_json(res, to_json(m_groups.get(id)), 201);
                }));

    server.Put("/api/groups/:id", guarded([this](httplib::Request const &req, httplib::Response &res) {
                   auto group = group_from_json(parse_object(req.body));
                   group.id = path_id(req);
                   m_groups.update(group);
                   send_json(res, to_json(m_groups.get(group.id)));
               }));

    server.Delete("/api/groups/:id", guarded([this](httplib::Request const &req, httplib::Response &res) {
                      m_groups.remove(path_id(req));
                      send_json(res, nlohmann::json::object());
                  }));
}
