#pragma once
#include <map>
#include <string>

#include "db.hpp"

/**
 * @brief Реквизиты организации и прочие настройки: пары ключ-значение.
 *
 * Известные ключи перечислены в SETTING_KEYS; значения проверяются по формату (ИНН, КПП, БИК, счета).
 * Методы не захватывают блокировку базы.
 */
class Settings {
   public:
    explicit Settings(Database &db);

    /// Все известные ключи; отсутствующие в базе - пустые строки.
    [[nodiscard]] std::map<std::string, std::string> all() const;

    /**
     * @brief Сохраняет переданные ключи, остальные не трогает.
     * @throws UserError неизвестный ключ или неверный формат значения
     */
    void update(std::map<std::string, std::string> const &values);

    /// Организация - плательщик НДС (ключ vat_payer = "1").
    [[nodiscard]] bool vat_payer() const;

   private:
    Database &m_db;
};

/**
 * @brief Проверка ИНН (10 цифр у организации, 12 у ИП) с контрольными числами; пустой допустим.
 * @throws UserError
 */
void check_inn(std::string const &inn);

/// КПП: пусто или 9 символов (цифры, 5-6 позиции могут быть буквами). @throws UserError
void check_kpp(std::string const &kpp);
