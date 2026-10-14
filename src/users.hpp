#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "db.hpp"

/**
 * @brief Роль пользователя; права растут от продавца к администратору.
 */
enum class Role : std::uint8_t {
    SELLER, ///< документы всех видов, кроме установки цен, и клиенты; остальное только просмотр
    TECHNOLOGIST, ///< всё, кроме пользователей
    ADMIN,        ///< всё и пользователи
};

[[nodiscard]] std::string_view to_string(Role role);
/// @throws UserError для неизвестного значения
[[nodiscard]] Role role_from_string(std::string_view text);

/**
 * @brief Пользователь.
 */
struct User {
    std::string login; ///< уникален без учёта регистра
    std::string name;  ///< как показывать в документах; пустое - логин
    std::int64_t id{};
    Role role = Role::SELLER;
    bool blocked = false; ///< заблокированный не может войти, его сессии закрыты
};

/// Сколько живёт сессия после входа, дней.
constexpr int SESSION_DAYS = 30;

/**
 * @brief Пользователи, пароли и сессии.
 *
 * Пароль хранится как PBKDF2-HMAC-SHA256 со случайной солью, сессия - как SHA-256 от случайного токена:
 * утечка базы не даёт ни паролей, ни действующих токенов. В базе всегда остаётся хотя бы один
 * незаблокированный администратор. Методы не захватывают блокировку базы.
 */
class Users {
   public:
    explicit Users(Database &db);

    /// В базе нет ни одного пользователя: первый создаётся через setup().
    [[nodiscard]] bool empty() const;

    /// По логину.
    [[nodiscard]] std::vector<User> list() const;

    /// @throws NotFoundError
    [[nodiscard]] User get(std::int64_t id) const;

    /**
     * @brief Первый пользователь - администратор; работает только в пустой базе.
     * @throws UserError пользователи уже есть, неверный логин или пароль
     * @return id
     */
    std::int64_t setup(User const &user, std::string_view password);

    /**
     * @brief Создаёт пользователя; логин и имя обрезаются по краям.
     * @throws UserError пустой, занятый или слишком длинный логин, короткий пароль
     * @return id
     */
    std::int64_t create(User const &user, std::string_view password);

    /**
     * @brief Меняет имя, роль и блокировку; блокировка закрывает сессии пользователя.
     * @throws NotFoundError, UserError не осталось бы ни одного действующего администратора
     */
    void update(User const &user);

    /**
     * @brief Задаёт пароль и закрывает все сессии пользователя.
     * @throws NotFoundError, UserError короткий пароль
     */
    void set_password(std::int64_t id, std::string_view password);

    /// Пароль подходит к пользователю id.
    [[nodiscard]] bool check_password(std::int64_t id, std::string_view password) const;

    /**
     * @brief Вход: проверяет логин и пароль и открывает сессию.
     * @throws UserError неверный логин или пароль, пользователь заблокирован
     * @return токен сессии для cookie
     */
    [[nodiscard]] std::string login(std::string_view login, std::string_view password);

    /// Открывает сессию пользователя id без проверки пароля (после setup и смены своего пароля).
    [[nodiscard]] std::string open_session(std::int64_t id);

    /// Пользователь действующей сессии или nullopt: нет такой, истекла, пользователь заблокирован.
    [[nodiscard]] std::optional<User> by_session(std::string_view token) const;

    /// Закрывает сессию; неизвестный токен ничего не делает.
    void logout(std::string_view token);

   private:
    /// Проверяет логин, имя и уникальность логина (кроме пользователя except).
    void validate(User &user, std::int64_t except) const;

    Database &m_db;
};
