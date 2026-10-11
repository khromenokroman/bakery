#include "settings.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <string_view>

#include "errors.hpp"

namespace {

/// Ключи настроек: организация, банк, подписи, НДС.
constexpr std::array<std::string_view, 15> SETTING_KEYS = {
    "org_name",     "org_inn",           "org_kpp",  "org_okpo",   "org_address", "org_phone", "bank_name",  "bank_bik",
    "bank_account", "bank_corr_account", "director", "accountant", "storekeeper", "vat_payer", "doc_footer",
};

bool all_digits(std::string const &s) {
    return std::all_of(s.begin(), s.end(), [](unsigned char c) { return std::isdigit(c) != 0; });
}

int inn_check(std::string const &inn, std::initializer_list<int> weights) {
    int sum = 0;
    std::size_t i = 0;
    for (auto const w : weights) {
        sum += w * (inn[i++] - '0');
    }
    return sum % 11 % 10;
}

void check_digits(std::string const &value, std::size_t size, std::string const &what) {
    if (!value.empty() && (value.size() != size || !all_digits(value))) {
        throw UserError(what + ": нужно " + std::to_string(size) + " цифр");
    }
}

} // namespace

void check_inn(std::string const &inn) {
    if (inn.empty()) {
        return;
    }
    if (!all_digits(inn) || (inn.size() != 10 && inn.size() != 12)) {
        throw UserError("ИНН «" + inn + "»: нужно 10 цифр (организация) или 12 (ИП)");
    }
    auto const ok = inn.size() == 10 ? inn_check(inn, {2, 4, 10, 3, 5, 9, 4, 6, 8}) == inn[9] - '0'
                                     : inn_check(inn, {7, 2, 4, 10, 3, 5, 9, 4, 6, 8}) == inn[10] - '0' &&
                                           inn_check(inn, {3, 7, 2, 4, 10, 3, 5, 9, 4, 6, 8}) == inn[11] - '0';
    if (!ok) {
        throw UserError("ИНН «" + inn + "»: не сходится контрольное число, проверьте цифры");
    }
}

void check_kpp(std::string const &kpp) {
    if (kpp.empty()) {
        return;
    }
    auto ok = kpp.size() == 9;
    for (std::size_t i = 0; ok && i < kpp.size(); ++i) {
        auto const c = static_cast<unsigned char>(kpp[i]);
        ok = std::isdigit(c) != 0 || ((i == 4 || i == 5) && std::isupper(c) != 0);
    }
    if (!ok) {
        throw UserError("КПП «" + kpp + "»: нужно 9 символов");
    }
}

Settings::Settings(Database &db) : m_db{db} {}

std::map<std::string, std::string> Settings::all() const {
    std::map<std::string, std::string> out;
    for (auto const key : SETTING_KEYS) {
        out[std::string(key)] = "";
    }
    auto stmt = m_db.prepare("SELECT key, value FROM settings");
    while (stmt.step()) {
        out[stmt.column_text(0)] = stmt.column_text(1);
    }
    return out;
}

void Settings::update(std::map<std::string, std::string> const &values) {
    for (auto const &[key, value] : values) {
        if (std::find(SETTING_KEYS.begin(), SETTING_KEYS.end(), key) == SETTING_KEYS.end()) {
            throw UserError("неизвестная настройка: " + key);
        }
        if (value.size() > 1000) {
            throw UserError("слишком длинное значение настройки " + key);
        }
    }
    auto const get = [&](std::string const &key) { return values.contains(key) ? values.at(key) : std::string{}; };
    check_inn(get("org_inn"));
    check_kpp(get("org_kpp"));
    check_digits(get("org_okpo"), get("org_okpo").size() == 10 ? 10 : 8, "ОКПО");
    check_digits(get("bank_bik"), 9, "БИК");
    check_digits(get("bank_account"), 20, "расчётный счёт");
    check_digits(get("bank_corr_account"), 20, "корреспондентский счёт");
    if (values.contains("vat_payer") && get("vat_payer") != "0" && get("vat_payer") != "1") {
        throw UserError("vat_payer: нужно 0 или 1");
    }

    Transaction tx{m_db};
    auto stmt = m_db.prepare("INSERT INTO settings (key, value) VALUES (?, ?) ON CONFLICT (key) DO UPDATE SET value = excluded.value");
    for (auto const &[key, value] : values) {
        stmt.reset();
        stmt.bind(1, key).bind(2, value);
        stmt.step();
    }
    tx.commit();
}

bool Settings::vat_payer() const { return all().at("vat_payer") == "1"; }
