#include "api.hpp"

#include <httplib.h>

#include <algorithm>
#include <charconv>
#include <exception>
#include <nlohmann/json.hpp>
#include <stdexcept>

#include "errors.hpp"
#include "json_io.hpp"
#include "period.hpp"
#include "report.hpp"
#include "repost.hpp"
#include "requirements.hpp"
#include "schema.hpp"
#include "settings.hpp"

namespace {

constexpr char const JSON_TYPE[] = "application/json; charset=utf-8";

/// Наибольший размер тела запроса: фотография с запасом; больше сервер не читает и отвечает 413.
constexpr std::size_t MAX_REQUEST_BYTES = 2 * MAX_PHOTO_BYTES;

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

/// Заголовок документа для ответов перепроведения: место в порядке проведения и название.
nlohmann::json doc_header(Document const &doc) {
    return {{"id", doc.id},     {"type", to_string(doc.type)}, {"number", doc.number},   {"date", doc.date},
            {"time", doc.time}, {"moment", moment(doc)},       {"title", doc_title(doc)}};
}

/// Момент из даты и времени запроса: "ГГГГ-ММ-ДД ЧЧ:ММ:СС"; время по умолчанию 00:00.
std::string parse_moment(std::string const &date, std::string const &time) {
    check_date(date);
    return date + " " + normalize_time(time.empty() ? "00:00" : time);
}

} // namespace

Api::Api(Database &db, std::string web_dir)
    : m_web_dir{std::move(web_dir)}, m_catalog{db}, m_groups{db}, m_prices{db}, m_partners{db}, m_docs{db}, m_stock{db}, m_db{db} {}

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
    server.set_payload_max_length(MAX_REQUEST_BYTES);
    server.Get("/api/version", guarded([this](httplib::Request const &, httplib::Response &res) {
                   send_json(res, {{"version", BAKERY_VERSION}, {"schema", current_schema_version(m_db)}});
               }));
    register_catalog(server);
    register_docs(server);
    register_groups(server);
    register_prices(server);
    register_partners(server);

    server.Get("/api/settings", guarded([this](httplib::Request const &, httplib::Response &res) { send_json(res, Settings{m_db}.all()); }));
    server.Put("/api/settings", guarded([this](httplib::Request const &req, httplib::Response &res) {
                   std::map<std::string, std::string> values;
                   // items() ссылается на объект: временный из parse_object уничтожился бы до цикла
                   auto const body = parse_object(req.body);
                   for (auto const &[key, value] : body.items()) {
                       if (!value.is_string()) {
                           throw UserError("настройка " + key + " должна быть строкой");
                       }
                       values[key] = value.get<std::string>();
                   }
                   Settings settings{m_db};
                   settings.update(values);
                   send_json(res, settings.all());
               }));

    auto const period_json = [this] {
        auto const till = Period{m_db}.closed_till();
        return nlohmann::json{{"closed_till", till ? nlohmann::json(*till) : nlohmann::json(nullptr)}};
    };
    server.Get("/api/period", guarded([period_json](httplib::Request const &, httplib::Response &res) { send_json(res, period_json()); }));
    server.Put("/api/period", guarded([this, period_json](httplib::Request const &req, httplib::Response &res) {
                   auto const j = parse_object(req.body);
                   std::optional<std::string> till;
                   if (j.contains("closed_till") && !j["closed_till"].is_null()) {
                       if (!j["closed_till"].is_string()) {
                           throw UserError("closed_till: нужна дата ГГГГ-ММ-ДД или null");
                       }
                       till = j["closed_till"].get<std::string>();
                   }
                   Period{m_db}.close(till);
                   send_json(res, period_json());
               }));

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
            send_error(res, res.status,
                       res.status == 404   ? "нет такого метода: " + req.method + " " + req.path
                       : res.status == 413 ? "запрос больше " + std::to_string(MAX_REQUEST_BYTES / 1024 / 1024) + " МБ"
                                           : "ошибка запроса");
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
                    item = item_from_json(j, item);
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

    server.Get("/api/items/:id/photo", guarded([this](httplib::Request const &req, httplib::Response &res) {
                   auto const photo = m_catalog.photo(path_id(req));
                   if (!photo) {
                       throw NotFoundError("у позиции нет фотографии");
                   }
                   // браузер проверяет кеш по ETag при каждом показе: фото могли заменить
                   auto const etag = "\"" + photo->etag + "\"";
                   res.set_header("ETag", etag);
                   res.set_header("Cache-Control", "no-cache");
                   if (req.get_header_value("If-None-Match") == etag) {
                       res.status = 304;
                       return;
                   }
                   res.set_content(photo->data, photo->mime);
               }));

    server.Put("/api/items/:id/photo", guarded([this](httplib::Request const &req, httplib::Response &res) {
                   auto const id = path_id(req);
                   m_catalog.set_photo(id, req.body);
                   send_json(res, to_json(m_catalog.get(id)));
               }));

    server.Delete("/api/items/:id/photo", guarded([this](httplib::Request const &req, httplib::Response &res) {
                      auto const id = path_id(req);
                      m_catalog.remove_photo(id);
                      send_json(res, to_json(m_catalog.get(id)));
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

    server.Get("/api/docs/:id/returnable", guarded([this](httplib::Request const &req, httplib::Response &res) {
                   std::optional<std::int64_t> exclude;
                   if (req.has_param("exclude")) {
                       exclude = query_id(req, "exclude");
                   }
                   auto out = nlohmann::json::array();
                   for (auto const &r : m_docs.returnable(path_id(req), exclude)) {
                       out.push_back(to_json(r));
                   }
                   send_json(res, out);
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

    server.Get(
        "/api/repost", guarded([this](httplib::Request const &, httplib::Response &res) {
            Reposter const reposter{m_db};
            auto const state = reposter.state();
            nlohmann::json out{{"needed", state.from.has_value()}, {"epoch", state.epoch}, {"from", nullptr}, {"cause", nullptr}, {"count", 0}};
            if (state.from) {
                out["from"] = {{"moment", state.from->moment}, {"id", state.from->id}};
                out["count"] = reposter.count(state.from->moment);
            }
            if (state.cause_id) {
                out["cause"] = doc_header(m_docs.get(*state.cause_id));
            }
            send_json(res, out);
        }));

    server.Get("/api/repost/count", guarded([this](httplib::Request const &req, httplib::Response &res) {
                   send_json(res, {{"count", Reposter{m_db}.count(parse_moment(req.get_param_value("date"), req.get_param_value("time")))}});
               }));

    server.Post("/api/repost/step", guarded([this](httplib::Request const &req, httplib::Response &res) {
                    auto const j = parse_object(req.body);
                    auto const from = parse_moment(j.value("date", ""), j.value("time", ""));
                    std::optional<DocKey> after;
                    if (j.contains("after") && j["after"].is_object()) {
                        auto const &a = j["after"];
                        after = DocKey{a.value("moment", ""), a.value("id", std::int64_t{0})};
                    }
                    auto const step = Reposter{m_db}.step(from, after, j.value("epoch", std::int64_t{0}));
                    nlohmann::json out{{"doc", nullptr}, {"error", nullptr}, {"remaining", step.remaining}, {"needed", step.needed}};
                    if (step.doc) {
                        out["doc"] = doc_header(*step.doc);
                    }
                    if (step.error) {
                        out["error"] = *step.error;
                    }
                    send_json(res, out);
                }));

    server.Get("/api/stock", guarded([this](httplib::Request const &req, httplib::Response &res) {
                   std::optional<std::string> date;
                   if (req.has_param("date")) {
                       check_date(req.get_param_value("date"));
                       date = req.get_param_value("date");
                       if (req.has_param("time")) {
                           *date += " " + normalize_time(req.get_param_value("time"));
                       }
                   }
                   std::optional<std::int64_t> exclude;
                   if (req.has_param("exclude")) {
                       exclude = query_id(req, "exclude");
                   }
                   auto out = nlohmann::json::array();
                   std::optional<std::int64_t> group;
                   if (req.has_param("group")) {
                       group = query_id(req, "group");
                       (void)m_groups.get(*group);
                   }
                   for (auto const &b : m_stock.balances(date, group, exclude)) {
                       out.push_back(to_json(b));
                   }
                   send_json(res, out);
               }));

    server.Get("/api/report/goods", guarded([this](httplib::Request const &req, httplib::Response &res) {
                   std::optional<std::int64_t> group;
                   if (req.has_param("group")) {
                       group = query_id(req, "group");
                       (void)m_groups.get(*group);
                   }
                   std::optional<std::int64_t> price_type;
                   if (req.has_param("price_type")) {
                       price_type = query_id(req, "price_type");
                       (void)m_prices.type(*price_type);
                   }
                   send_json(res, to_json(goods_report(m_db, req.get_param_value("from"), req.get_param_value("to"), group, price_type)));
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

void Api::register_prices(httplib::Server &server) {
    server.Get("/api/price-types", guarded([this](httplib::Request const &, httplib::Response &res) {
                   auto out = nlohmann::json::array();
                   for (auto const &t : m_prices.types()) {
                       out.push_back(to_json(t));
                   }
                   send_json(res, out);
               }));

    server.Post("/api/price-types", guarded([this](httplib::Request const &req, httplib::Response &res) {
                    auto const id = m_prices.create_type(get_string(parse_object(req.body), "name"));
                    send_json(res, to_json(m_prices.type(id)), 201);
                }));

    server.Put("/api/price-types/:id", guarded([this](httplib::Request const &req, httplib::Response &res) {
                   auto const id = path_id(req);
                   m_prices.rename_type(id, get_string(parse_object(req.body), "name"));
                   send_json(res, to_json(m_prices.type(id)));
               }));

    server.Delete("/api/price-types/:id", guarded([this](httplib::Request const &req, httplib::Response &res) {
                      m_prices.remove_type(path_id(req));
                      send_json(res, nlohmann::json::object());
                  }));

    server.Get("/api/prices", guarded([this](httplib::Request const &req, httplib::Response &res) {
                   if (!req.has_param("type")) {
                       throw UserError("не указан тип цен (type)");
                   }
                   auto const type = query_id(req, "type");
                   (void)m_prices.type(type);
                   auto const date = req.get_param_value("date");
                   check_date(date);
                   auto const when = req.has_param("time") ? date + " " + normalize_time(req.get_param_value("time")) : date;
                   auto out = nlohmann::json::array();
                   for (auto const &row : m_prices.price_list(type, when)) {
                       out.push_back(to_json(row));
                   }
                   send_json(res, out);
               }));
}

void Api::register_partners(httplib::Server &server) {
    server.Get("/api/partners", guarded([this](httplib::Request const &req, httplib::Response &res) {
                   auto out = nlohmann::json::array();
                   for (auto const &p : m_partners.list(req.get_param_value("archived") == "1")) {
                       out.push_back(to_json(p));
                   }
                   send_json(res, out);
               }));

    server.Post("/api/partners", guarded([this](httplib::Request const &req, httplib::Response &res) {
                    auto const j = parse_object(req.body);
                    (void)get_string(j, "name");
                    auto const id = m_partners.create(partner_from_json(j, {}));
                    send_json(res, to_json(m_partners.get(id)), 201);
                }));

    server.Get("/api/partners/:id",
               guarded([this](httplib::Request const &req, httplib::Response &res) { send_json(res, to_json(m_partners.get(path_id(req)))); }));

    server.Put("/api/partners/:id", guarded([this](httplib::Request const &req, httplib::Response &res) {
                   auto const id = path_id(req);
                   m_partners.update(partner_from_json(parse_object(req.body), m_partners.get(id)));
                   send_json(res, to_json(m_partners.get(id)));
               }));

    server.Delete("/api/partners/:id", guarded([this](httplib::Request const &req, httplib::Response &res) {
                      m_partners.remove(path_id(req));
                      send_json(res, nlohmann::json::object());
                  }));
}
