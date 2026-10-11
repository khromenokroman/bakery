#pragma once
#include <string>

#include "db.hpp"

namespace httplib {
class Server;
}

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
    std::string m_web_dir;
    Database &m_db;
};
