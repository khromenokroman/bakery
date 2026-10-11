#pragma once
#include <functional>
#include <string>

#include "catalog.hpp"
#include "db.hpp"
#include "docs.hpp"
#include "stock.hpp"

namespace httplib {
class Server;
struct Request;
struct Response;
} // namespace httplib

/**
 * @brief HTTP API и раздача веб-интерфейса.
 *
 * Все ответы /api/ в JSON; ошибки возвращаются как {"error": "текст"}.
 */
class Api {
   public:
    /**
     * @param db база, к которой обращаются обработчики
     * @param web_dir каталог статических страниц; пустой, если раздавать не нужно
     */
    Api(Database &db, std::string web_dir);

    /// Регистрирует обработчики на сервере.
    void register_routes(httplib::Server &server);

   private:
    using Handler = std::function<void(httplib::Request const &, httplib::Response &)>;

    /**
     * @brief Оборачивает обработчик: захват базы, перевод исключений в ответы 400/404.
     */
    [[nodiscard]] Handler guarded(Handler handler);

    void register_catalog(httplib::Server &server);
    void register_docs(httplib::Server &server);

    std::string m_web_dir;
    Catalog m_catalog;
    Documents m_docs;
    Stock m_stock;
    Database &m_db;
};
