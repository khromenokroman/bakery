#pragma once
#include <stdexcept>

/**
 * @brief Ошибка в данных от пользователя; текст показывается в интерфейсе как есть (HTTP 400).
 */
class UserError : public std::runtime_error {
   public:
    using std::runtime_error::runtime_error;
};

/**
 * @brief Запрошенный объект не найден (HTTP 404).
 */
class NotFoundError : public std::runtime_error {
   public:
    using std::runtime_error::runtime_error;
};

/**
 * @brief Запрос без входа или с истёкшей сессией (HTTP 401).
 */
class UnauthorizedError : public std::runtime_error {
   public:
    using std::runtime_error::runtime_error;
};

/**
 * @brief Роли пользователя не хватает прав (HTTP 403).
 */
class ForbiddenError : public std::runtime_error {
   public:
    using std::runtime_error::runtime_error;
};
