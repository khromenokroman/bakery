#pragma once
#include <cstdint>
#include <functional>
#include <optional>
#include <string>

#include "catalog.hpp"
#include "db.hpp"
#include "docs.hpp"
#include "groups.hpp"
#include "partners.hpp"
#include "prices.hpp"
#include "stock.hpp"
#include "users.hpp"

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
     * @brief Кому доступен метод.
     */
    enum class Access : std::uint8_t {
        PUBLIC,       ///< без входа
        USER,         ///< любой вошедший пользователь
        TECHNOLOGIST, ///< технолог и администратор
        ADMIN,        ///< только администратор
    };

    /**
     * @brief Оборачивает обработчик: захват базы, проверка сессии и роли, перевод исключений в ответы
     * 400/401/403/404. На время обработчика m_user - вошедший пользователь, он же автор изменений документов.
     */
    [[nodiscard]] Handler guarded(Access access, Handler handler);

    /// То же для любого вошедшего пользователя.
    [[nodiscard]] Handler guarded(Handler handler);

    /// @throws ForbiddenError если продавец меняет документ вида type, который ему не доступен
    void check_doc_access(DocType type) const;

    void register_auth(httplib::Server &server);

    void register_catalog(httplib::Server &server);
    void register_docs(httplib::Server &server);
    void register_groups(httplib::Server &server);
    void register_prices(httplib::Server &server);
    void register_partners(httplib::Server &server);

    std::optional<User> m_user; ///< вошедший пользователь текущего запроса; меняется только под блокировкой базы
    std::string m_web_dir;
    Catalog m_catalog;
    Groups m_groups;
    Prices m_prices;
    Partners m_partners;
    Documents m_docs;
    Stock m_stock;
    Database &m_db;
};
