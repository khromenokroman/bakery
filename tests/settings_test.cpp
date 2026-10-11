#include "settings.hpp"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "errors.hpp"
#include "fixture.hpp"
#include "partners.hpp"
#include "test_util.hpp"

using testing::StrEq;

/**
 * @brief ИНН: длина 10 или 12, цифры, контрольные числа; пустой допустим.
 */
TEST(Requisites, Inn) {
    EXPECT_NO_THROW(check_inn("7707083893")) << "ИНН организации с верным контрольным числом";
    EXPECT_NO_THROW(check_inn("500100732259")) << "ИНН ИП с верными контрольными числами";
    EXPECT_NO_THROW(check_inn(""));
    EXPECT_THROW_REASON(check_inn("7707083894"), UserError, "не сходится контрольное число");
    EXPECT_THROW_REASON(check_inn("500100732258"), UserError, "не сходится контрольное число");
    EXPECT_THROW_REASON(check_inn("12345"), UserError, "нужно 10 цифр (организация) или 12 (ИП)");
    EXPECT_THROW_REASON(check_inn("77070838a3"), UserError, "нужно 10 цифр");
}

/**
 * @brief КПП: 9 символов, буквы допустимы только в 5-6 позициях.
 */
TEST(Requisites, Kpp) {
    EXPECT_NO_THROW(check_kpp("773601001"));
    EXPECT_NO_THROW(check_kpp("7736AB001"));
    EXPECT_THROW_REASON(check_kpp("77360100"), UserError, "нужно 9 символов");
    EXPECT_THROW_REASON(check_kpp("A73601001"), UserError, "нужно 9 символов");
}

namespace {

class SettingsTest : public DbFixture {
   protected:
    Settings m_settings{m_db};
};

} // namespace

/**
 * @brief Настройки сохраняются частично, неизвестный ключ и неверные реквизиты отклоняются.
 */
TEST_F(SettingsTest, UpdateAndValidate) {
    EXPECT_THAT(m_settings.all().at("org_name"), StrEq("")) << "незаполненная настройка - пустая строка";
    EXPECT_FALSE(m_settings.vat_payer());
    m_settings.update({{"org_name", "ООО «Пекарня»"}, {"org_inn", "7707083893"}, {"vat_payer", "1"}});
    m_settings.update({{"director", "Иванов И. И."}});
    auto const all = m_settings.all();
    EXPECT_THAT(all.at("org_name"), StrEq("ООО «Пекарня»")) << "прежние ключи не стираются";
    EXPECT_THAT(all.at("director"), StrEq("Иванов И. И."));
    EXPECT_TRUE(m_settings.vat_payer());

    EXPECT_THROW_REASON(m_settings.update({{"color", "red"}}), UserError, "неизвестная настройка: color");
    EXPECT_THROW_REASON(m_settings.update({{"bank_bik", "12345"}}), UserError, "БИК: нужно 9 цифр");
    EXPECT_THROW_REASON(m_settings.update({{"bank_account", "4070281000000000000"}}), UserError, "расчётный счёт: нужно 20 цифр");
    EXPECT_THROW_REASON(m_settings.update({{"vat_payer", "да"}}), UserError, "нужно 0 или 1");
    EXPECT_THROW_REASON(m_settings.update({{"org_inn", "1234567890"}}), UserError, "ИНН");
}

/**
 * @brief Ставка НДС у товара: допустимые значения и «без НДС».
 */
TEST_F(SettingsTest, ItemVatRate) {
    Item item;
    item.name = "Хлеб";
    item.kind = Kind::PRODUCT;
    item.unit = Unit::PCS;
    item.vat_rate = 10;
    auto const id = m_catalog.create(item);
    EXPECT_EQ(m_catalog.get(id).vat_rate, 10);
    auto saved = m_catalog.get(id);
    saved.vat_rate = std::nullopt;
    m_catalog.update(saved);
    EXPECT_FALSE(m_catalog.get(id).vat_rate) << "без НДС";
    saved.vat_rate = 18;
    EXPECT_THROW_REASON(m_catalog.update(saved), UserError, "ставка НДС 18%: допустимы 0, 5, 7, 10, 20, 22");
}

/**
 * @brief Реквизиты клиента сохраняются и проверяются.
 */
TEST_F(SettingsTest, PartnerRequisites) {
    Partners partners{m_db};
    Partner p;
    p.name = "ООО «Колосок»";
    p.inn = " 7707083893 ";
    p.kpp = "773601001";
    p.address = "г. Москва, ул. Ленина, 1";
    auto const id = partners.create(p);
    auto const saved = partners.get(id);
    EXPECT_THAT(saved.inn, StrEq("7707083893"));
    EXPECT_THAT(saved.address, StrEq("г. Москва, ул. Ленина, 1"));
    p.name = "Другой";
    p.inn = "7707083894";
    EXPECT_THROW_REASON(partners.create(p), UserError, "не сходится контрольное число");
}
