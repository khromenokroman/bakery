#include "api.hpp"

#include <httplib.h>

#include <exception>
#include <nlohmann/json.hpp>
#include <stdexcept>

#include "schema.hpp"

namespace {

constexpr char const JSON_TYPE[] = "application/json; charset=utf-8";

void send_json(httplib::Response &res, nlohmann::json const &body, int status = 200) {
    res.status = status;
    res.set_content(body.dump(), JSON_TYPE);
}

void send_error(httplib::Response &res, int status, std::string const &message) { send_json(res, {{"error", message}}, status); }

} // namespace

Api::Api(Database &db, std::string web_dir) : m_web_dir{std::move(web_dir)}, m_db{db} {}

void Api::register_routes(httplib::Server &server) {
    server.Get("/api/version", [this](httplib::Request const &, httplib::Response &res) {
        auto const lock = m_db.lock();
        send_json(res, {{"version", BAKERY_VERSION}, {"schema", current_schema_version(m_db)}});
    });

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
