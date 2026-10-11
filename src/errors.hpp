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
