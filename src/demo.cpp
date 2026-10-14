#include "demo.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <map>
#include <optional>
#include <random>
#include <string_view>
#include <vector>

#include "catalog.hpp"
#include "docs.hpp"
#include "errors.hpp"
#include "groups.hpp"
#include "partners.hpp"
#include "prices.hpp"
#include "settings.hpp"
#include "users.hpp"

namespace {

using namespace std::chrono;

/// Дата "ГГГГ-ММ-ДД" со сдвигом на days дней.
std::string shift(std::string const &date, int days) {
    int y = 0;
    unsigned m = 0;
    unsigned d = 0;
    std::sscanf(date.c_str(), "%d-%u-%u", &y, &m, &d);
    year_month_day const ymd{sys_days{year{y} / month{m} / day{d}} + std::chrono::days{days}};
    char text[11];
    std::snprintf(text, sizeof text, "%04d-%02u-%02u", static_cast<int>(ymd.year()), static_cast<unsigned>(ymd.month()),
                  static_cast<unsigned>(ymd.day()));
    return text;
}

std::string hhmm(int minutes) {
    char text[6];
    std::snprintf(text, sizeof text, "%02d:%02d", minutes / 60, minutes % 60);
    return text;
}

/**
 * @brief Детерминированный генератор: одинаковые данные на любой платформе (распределения стандартной
 * библиотеки могут отличаться, поэтому только сырой выход mt19937).
 */
class Random {
   public:
    /// Целое в [lo, hi].
    std::int64_t between(std::int64_t lo, std::int64_t hi) {
        return lo + static_cast<std::int64_t>(m_gen() % static_cast<std::uint32_t>(hi - lo + 1));
    }
    /// true с вероятностью percent %.
    bool chance(int percent) { return between(1, 100) <= percent; }
    template <typename T>
    T const &pick(std::vector<T> const &list) {
        return list[static_cast<std::size_t>(between(0, static_cast<std::int64_t>(list.size()) - 1))];
    }

   private:
    std::mt19937 m_gen{20261014};
};

/// Сырьё: название, единица, себестоимость за единицу (копейки), к ней привязываются начинки.
struct RawSpec {
    char const *name;
    Unit unit;
};

constexpr RawSpec RAWS[] = {
    {"Мука пшеничная высший сорт", Unit::KG},
    {"Мука пшеничная первый сорт", Unit::KG},
    {"Мука ржаная обдирная", Unit::KG},
    {"Мука ржаная сеяная", Unit::KG},
    {"Отруби пшеничные", Unit::KG},
    {"Сахар", Unit::KG},
    {"Сахарная пудра", Unit::KG},
    {"Соль", Unit::KG},
    {"Дрожжи прессованные", Unit::KG},
    {"Закваска ржаная", Unit::KG},
    {"Солод ржаной", Unit::KG},
    {"Масло сливочное", Unit::KG},
    {"Масло подсолнечное", Unit::L},
    {"Маргарин", Unit::KG},
    {"Молоко", Unit::L},
    {"Сливки 33%", Unit::L},
    {"Творог", Unit::KG},
    {"Сыр", Unit::KG},
    {"Яйцо куриное", Unit::PCS},
    {"Мак пищевой", Unit::KG},
    {"Изюм", Unit::KG},
    {"Кунжут", Unit::KG},
    {"Семечки подсолнечника", Unit::KG},
    {"Корица", Unit::KG},
    {"Какао-порошок", Unit::KG},
    {"Шоколад", Unit::KG},
    {"Орехи грецкие", Unit::KG},
    {"Мёд", Unit::KG},
    {"Вишня", Unit::KG},
    {"Яблоки", Unit::KG},
    {"Капуста", Unit::KG},
    {"Картофель", Unit::KG},
    {"Фарш мясной", Unit::KG},
    {"Филе куриное", Unit::KG},
    {"Ветчина", Unit::KG},
    {"Грибы", Unit::KG},
    {"Рис", Unit::KG},
    {"Лук репчатый", Unit::KG},
    {"Тмин", Unit::KG},
};

/// Полуфабрикат: название и состав из сырья на 1 кг выхода (граммы или штуки ×1000).
struct SemiSpec {
    char const *name;
    std::vector<std::pair<char const *, std::int64_t>> parts;
};

std::vector<SemiSpec> const SEMIS = {
    {"Тесто дрожжевое", {{"Мука пшеничная высший сорт", 600}, {"Дрожжи прессованные", 15}, {"Соль", 10}, {"Сахар", 20}, {"Молоко", 300}}},
    {"Тесто сдобное",
     {{"Мука пшеничная высший сорт", 550}, {"Сахар", 120}, {"Масло сливочное", 100}, {"Яйцо куриное", 2000}, {"Дрожжи прессованные", 20}}},
    {"Тесто слоёное", {{"Мука пшеничная высший сорт", 500}, {"Маргарин", 400}, {"Соль", 8}}},
    {"Тесто ржаное", {{"Мука ржаная обдирная", 450}, {"Мука пшеничная первый сорт", 150}, {"Закваска ржаная", 100}, {"Соль", 12}}},
    {"Тесто заварное ржаное", {{"Мука ржаная сеяная", 500}, {"Солод ржаной", 50}, {"Закваска ржаная", 80}, {"Тмин", 5}}},
    {"Тесто песочное", {{"Мука пшеничная высший сорт", 500}, {"Масло сливочное", 300}, {"Сахар", 200}}},
    {"Тесто бисквитное", {{"Мука пшеничная высший сорт", 300}, {"Сахар", 300}, {"Яйцо куриное", 6000}}},
    {"Тесто заварное", {{"Мука пшеничная высший сорт", 250}, {"Масло сливочное", 200}, {"Яйцо куриное", 5000}}},
    {"Тесто для пиццы", {{"Мука пшеничная высший сорт", 600}, {"Масло подсолнечное", 50}, {"Дрожжи прессованные", 10}, {"Соль", 10}}},
    {"Тесто с отрубями", {{"Мука пшеничная первый сорт", 450}, {"Отруби пшеничные", 150}, {"Дрожжи прессованные", 15}, {"Соль", 10}}},
    {"Крем заварной", {{"Молоко", 700}, {"Сахар", 200}, {"Яйцо куриное", 4000}, {"Мука пшеничная высший сорт", 50}}},
    {"Крем сливочный", {{"Сливки 33%", 700}, {"Сахарная пудра", 300}}},
    {"Крем масляный", {{"Масло сливочное", 600}, {"Сахарная пудра", 400}}},
    {"Крем шоколадный", {{"Сливки 33%", 500}, {"Шоколад", 400}, {"Сахар", 100}}},
    {"Начинка капустная", {{"Капуста", 900}, {"Лук репчатый", 80}, {"Масло подсолнечное", 30}}},
    {"Начинка картофельная", {{"Картофель", 900}, {"Лук репчатый", 80}, {"Масло подсолнечное", 30}}},
    {"Начинка мясная", {{"Фарш мясной", 850}, {"Лук репчатый", 150}}},
    {"Начинка творожная", {{"Творог", 800}, {"Сахар", 150}, {"Яйцо куриное", 1000}}},
    {"Начинка вишнёвая", {{"Вишня", 850}, {"Сахар", 150}}},
    {"Начинка яблочная", {{"Яблоки", 850}, {"Сахар", 120}, {"Корица", 5}}},
    {"Начинка маковая", {{"Мак пищевой", 600}, {"Сахар", 300}, {"Молоко", 100}}},
    {"Начинка грибная", {{"Грибы", 800}, {"Лук репчатый", 150}, {"Масло подсолнечное", 50}}},
    {"Начинка рис с яйцом", {{"Рис", 500}, {"Яйцо куриное", 6000}, {"Масло сливочное", 50}}},
    {"Глазурь шоколадная", {{"Шоколад", 700}, {"Сливки 33%", 300}}},
    {"Карамель", {{"Сахар", 800}, {"Сливки 33%", 200}}},
};

/// Начинка или добавка по словам в названии.
constexpr std::pair<char const *, char const *> FILLINGS[] = {
    {"с капустой", "Начинка капустная"},
    {"с картофелем", "Начинка картофельная"},
    {"с мясом", "Начинка мясная"},
    {"с творогом", "Начинка творожная"},
    {"с вишней", "Начинка вишнёвая"},
    {"с яблоком", "Начинка яблочная"},
    {"с маком", "Начинка маковая"},
    {"с грибами", "Начинка грибная"},
    {"с рисом", "Начинка рис с яйцом"},
    {"с яйцом", "Начинка рис с яйцом"},
    {"заварным кремом", "Крем заварной"},
    {"сливочным кремом", "Крем сливочный"},
    {"с кремом", "Крем масляный"},
    {"шоколад", "Глазурь шоколадная"},
    {"с карамелью", "Карамель"},
    {"с изюмом", "Изюм"},
    {"с кунжутом", "Кунжут"},
    {"с семечками", "Семечки подсолнечника"},
    {"с орехами", "Орехи грецкие"},
    {"с корицей", "Корица"},
    {"с сыром", "Сыр"},
    {"с ветчиной", "Ветчина"},
    {"с курицей", "Филе куриное"},
    {"с ягодами", "Вишня"},
    {"с повидлом", "Начинка яблочная"},
    {"с джемом", "Начинка вишнёвая"},
    {"с сахаром", "Сахар"},
    {"с тмином", "Тмин"},
    {"медов", "Мёд"},
    {"с травами", "Масло подсолнечное"},
    {"с оливками", "Масло подсолнечное"},
    {"с томатами", "Масло подсолнечное"},
    {"с луком", "Лук репчатый"},
    {"с овощами", "Начинка картофельная"},
};

/// Категория продукции: группа, единица, тесто, себестоимость за штуку или кг, названия.
struct Category {
    char const *group;
    char const *parent; ///< nullptr - верхний уровень
    Unit unit;
    char const *dough;
    std::int64_t cost; ///< копейки за единицу при среднем размере
    std::vector<char const *> bases;
    std::vector<char const *> mods;
    std::vector<std::pair<char const *, int>> sizes; ///< вес (г) для штучного; пусто - один размер
};

std::vector<Category> const CATEGORIES = {
    {"Белый хлеб",
     "Хлеб",
     Unit::PCS,
     "Тесто дрожжевое",
     2800,
     {"Батон", "Хлеб пшеничный", "Булка городская", "Плетёнка", "Хлеб тостовый"},
     {"", "нарезной", "с кунжутом", "на молоке", "с семечками", "с изюмом"},
     {{"300 г", 300}, {"500 г", 500}}},
    {"Ржаной хлеб",
     "Хлеб",
     Unit::PCS,
     "Тесто ржаное",
     3200,
     {"Хлеб ржаной", "Хлеб бородинский", "Хлеб дарницкий", "Хлеб заварной"},
     {"", "с тмином", "на закваске", "с отрубями", "нарезка"},
     {{"400 г", 400}, {"700 г", 700}}},
    {"Багеты и чиабатта",
     "Хлеб",
     Unit::PCS,
     "Тесто дрожжевое",
     4000,
     {"Багет", "Чиабатта", "Фокачча"},
     {"", "с оливками", "с травами", "с сыром", "с томатами", "с луком"},
     {}},
    {"Сдоба",
     "Выпечка",
     Unit::PCS,
     "Тесто сдобное",
     2200,
     {"Булочка", "Плюшка", "Ватрушка", "Рогалик", "Сочень"},
     {"", "с маком", "с корицей", "с творогом", "с повидлом", "с изюмом", "с сахаром", "с кремом"},
     {}},
    {"Слойки",
     "Выпечка",
     Unit::PCS,
     "Тесто слоёное",
     3000,
     {"Слойка", "Круассан", "Конвертик"},
     {"", "с вишней", "с яблоком", "с ветчиной и сыром", "с шоколадом", "с творогом", "с курицей", "с маком"},
     {}},
    {"Пирожки",
     "Выпечка",
     Unit::PCS,
     "Тесто дрожжевое",
     1900,
     {"Пирожок печёный", "Пирожок жареный", "Расстегай", "Кулебяка"},
     {"с капустой", "с картофелем", "с мясом", "с яйцом и луком", "с рисом", "с грибами", "с яблоком", "с вишней"},
     {}},
    {"Торты",
     "Кондитерские изделия",
     Unit::KG,
     "Тесто бисквитное",
     52000,
     {"Торт «Наполеон»", "Торт «Медовик»", "Торт «Прага»", "Торт «Птичье молоко»", "Торт бисквитный", "Торт творожный"},
     {"", "с ягодами", "с орехами", "с карамелью"},
     {}},
    {"Пирожные",
     "Кондитерские изделия",
     Unit::PCS,
     "Тесто заварное",
     4500,
     {"Эклер", "Пирожное «Картошка»", "Корзиночка", "Профитроль", "Трубочка"},
     {"", "с заварным кремом", "со сливочным кремом", "с ягодами", "с карамелью"},
     {}},
    {"Печенье и пряники",
     "Кондитерские изделия",
     Unit::KG,
     "Тесто песочное",
     26000,
     {"Печенье овсяное", "Печенье песочное", "Печенье сахарное", "Пряник медовый", "Курабье"},
     {"", "с шоколадом", "с орехами", "с изюмом", "с кунжутом", "с джемом"},
     {}},
    {"Снеки",
     nullptr,
     Unit::PCS,
     "Тесто для пиццы",
     6500,
     {"Пицца", "Сэндвич", "Хачапури", "Самса", "Кальцоне"},
     {"с курицей", "с ветчиной", "с сыром", "с грибами", "с овощами"},
     {}},
};

constexpr char const *SHOP_NAMES[] = {
    "Колосок",  "Каравай", "Пятёрочка у дома", "Продукты 24", "Хлебница", "Берёзка",  "Ромашка", "Удача", "Семейный", "Солнечный",
    "Магнолия", "Радуга",  "Огонёк",           "Перекрёсток", "Зерно",    "Северный", "Луч",     "Весна", "Уют",      "Добрый"};
constexpr char const *CAFE_NAMES[] = {"Утро",    "Чашка",   "Кофейня на углу", "Блинная",    "Шоколадница", "Кофе и булка", "Столовая № 1", "Обед",
                                      "Трапеза", "Самовар", "Пончик",          "Вареничная", "Бистро",      "Гриль",        "Летний сад"};
constexpr char const *SURNAMES[] = {"Петрова",   "Иванов",   "Смирнова", "Кузнецов",  "Попова",   "Васильев", "Соколова", "Михайлов", "Новикова",
                                    "Фёдоров",   "Морозова", "Волков",   "Алексеева", "Лебедев",  "Семёнова", "Егоров",   "Павлова",  "Козлов",
                                    "Степанова", "Николаев", "Орлова",   "Андреев",   "Макарова", "Захаров",  "Зайцева"};
constexpr char const *WRITEOFF_REASONS[] = {"истёк срок годности", "брак при выпечке", "повреждена упаковка", "деформация при транспортировке",
                                            "возврат с витрины, черствый"};

/// Продукция с её ценами и тем, как часто она выпекается.
struct Product {
    std::int64_t id{};
    std::int64_t cost{};   ///< себестоимость, копейки за единицу
    std::int64_t retail{}; ///< розница
    std::int64_t opt{};    ///< опт
    int every{};           ///< выпекается раз в every дней
    int batch{};           ///< размер партии в штуках или кг
    Unit unit = Unit::PCS;
};

/// Округление цены до 10 копеек (штучное) или до рубля (за кг).
std::int64_t round_price(std::int64_t kop, Unit unit) {
    auto const step = unit == Unit::KG ? 100 : 10;
    return (kop + step / 2) / step * step;
}

/**
 * @brief Генератор: справочники, затем дни по порядку; остатки ведутся здесь же, чтобы не уходить в минус.
 */
class Demo {
   public:
    Demo(Database &db, std::string today, int days) : m_db{db}, m_docs{db}, m_today{std::move(today)}, m_days{days} {}

    DemoStats run() {
        users();
        settings();
        catalog();
        partners();
        prices(shift(m_today, -m_days), 0);
        for (int day = m_days - 1; day >= 0; --day) {
            one_day(shift(m_today, -day), day == 0);
        }
        // признак демонстрационной базы: полоса вверху страниц и подсказка входа
        m_db.exec("INSERT INTO settings (key, value) VALUES ('demo', '1') ON CONFLICT (key) DO UPDATE SET value = excluded.value");
        m_stats.items = static_cast<std::int64_t>(m_raw.size() + m_semi.size() + m_products.size());
        m_stats.partners = static_cast<std::int64_t>(m_partners.size());
        return m_stats;
    }

   private:
    void users() {
        Users users{m_db};
        auto const make = [&](char const *login, char const *name, Role role) {
            User u;
            u.login = login;
            u.name = name;
            u.role = role;
            return users.create(u, DEMO_PASSWORD);
        };
        m_stats.admin_id = make("admin", "Администратор", Role::ADMIN);
        m_tech = make("tech", "Татьяна Орлова", Role::TECHNOLOGIST);
        m_seller = make("seller", "Ольга Смирнова", Role::SELLER);
    }

    void settings() {
        Settings{m_db}.update({{"org_name", "ООО «Пекарня Демо»"},
                               {"org_address", "г. Тверь, ул. Хлебная, д. 1"},
                               {"org_phone", "+7 (4822) 00-00-00"},
                               {"director", "Иванова А. П."},
                               {"accountant", "Кузнецова Е. В."},
                               {"storekeeper", "Смирнова О. С."},
                               {"doc_footer", "Демонстрационные данные"}});
    }

    std::int64_t group(char const *name, std::optional<std::int64_t> parent) {
        auto const it = m_groups.find(name);
        if (it != m_groups.end()) {
            return it->second;
        }
        Group g;
        g.name = name;
        g.parent_id = parent;
        return m_groups[name] = Groups{m_db}.create(g);
    }

    std::int64_t item(std::string const &name, Kind kind, Unit unit, std::optional<std::int64_t> group_id) {
        Item i;
        i.name = name;
        i.kind = kind;
        i.unit = unit;
        i.group_id = group_id;
        return m_catalog.create(i);
    }

    void catalog() {
        auto const raw_group = group("Сырьё", std::nullopt);
        auto const semi_group = group("Полуфабрикаты", std::nullopt);
        for (auto const &r : RAWS) {
            m_raw[r.name] = item(r.name, Kind::RAW, r.unit, raw_group);
        }
        for (auto const &s : SEMIS) {
            auto const id = item(s.name, Kind::SEMI, Unit::KG, semi_group);
            m_semi[s.name] = id;
            Recipe recipe;
            recipe.output_qty = 1000;
            for (auto const &[part, qty] : s.parts) {
                recipe.lines.push_back({.component_id = m_raw.at(part), .qty = qty});
            }
            m_catalog.set_recipe(id, recipe);
        }
        for (auto const &c : CATEGORIES) {
            auto const parent = c.parent ? std::optional(group(c.parent, std::nullopt)) : std::nullopt;
            auto const gid = group(c.group, parent);
            m_category_group.push_back(gid);
            auto const sizes = c.sizes.empty() ? std::vector<std::pair<char const *, int>>{{"", 0}} : c.sizes;
            for (auto const *base : c.bases) {
                for (auto const *mod : c.mods) {
                    for (auto const &[size, grams] : sizes) {
                        std::string name = base;
                        if (*mod) {
                            name += std::string(" ") + mod;
                        }
                        if (*size) {
                            name += std::string(", ") + size;
                        }
                        product(name, c, gid, grams);
                    }
                }
            }
        }
    }

    void product(std::string const &name, Category const &c, std::int64_t gid, int grams) {
        Product p;
        p.unit = c.unit;
        p.id = item(name, Kind::PRODUCT, c.unit, gid);
        // штучное: тесто по весу изделия (по умолчанию 100 г), весовое: на 1 кг
        auto const weight = c.unit == Unit::KG ? 1000 : (grams ? grams : 100 + m_rand.between(0, 4) * 20);
        Recipe recipe;
        recipe.output_qty = 1000;
        auto const filling =
            std::find_if(std::begin(FILLINGS), std::end(FILLINGS), [&](auto const &f) { return name.find(f.first) != std::string::npos; });
        auto const dough_share = filling != std::end(FILLINGS) ? 75 : 100;
        recipe.lines.push_back({.component_id = m_semi.at(c.dough), .qty = weight * dough_share / 100});
        if (filling != std::end(FILLINGS)) {
            auto const semi = m_semi.find(filling->second);
            auto const component = semi != m_semi.end() ? semi->second : m_raw.at(filling->second);
            auto const is_egg = std::string_view(filling->second) == "Яйцо куриное";
            recipe.lines.push_back({.component_id = component, .qty = is_egg ? 1000 : weight * (100 - dough_share) / 100});
        }
        m_catalog.set_recipe(p.id, recipe);

        auto const size_factor = c.unit == Unit::KG || !grams ? 100 : grams * 100 / 400;
        p.cost = round_price(c.cost * size_factor / 100 * m_rand.between(85, 120) / 100, c.unit);
        p.retail = round_price(p.cost * m_rand.between(170, 210) / 100, c.unit);
        p.opt = round_price(p.retail * m_rand.between(78, 86) / 100, c.unit);
        p.every = static_cast<int>(m_rand.pick(std::vector<std::int64_t>{1, 1, 1, 2, 2, 3, 7}));
        p.batch = c.unit == Unit::KG ? static_cast<int>(m_rand.between(1, 4)) : static_cast<int>(m_rand.between(1, 4) * 5);
        m_products.push_back(p);
    }

    void partners() {
        Partners partners{m_db};
        auto const add = [&](std::string const &name, bool wholesale) {
            Partner p;
            p.name = name;
            p.phone = "+7 (900) " + std::to_string(100 + m_partners.size()) + "-" + std::to_string(10 + m_partners.size() % 90) + "-00";
            p.address = "г. Тверь, ул. " +
                        std::string(m_rand.pick(
                            std::vector<char const *>{"Советская", "Луначарского", "Трёхсвятская", "Склизкова", "Горького", "Вагжанова"})) +
                        ", д. " + std::to_string(1 + m_partners.size());
            m_partners.push_back({partners.create(p), wholesale});
            (wholesale ? m_wholesale_partners : m_retail_partners).push_back(m_partners.back());
        };
        add("Розничный покупатель", false);
        for (auto const *n : SHOP_NAMES) {
            add(std::string("Магазин «") + n + "»", true);
        }
        for (auto const *n : CAFE_NAMES) {
            add(std::string("Кафе «") + n + "»", true);
        }
        for (int i = 1; i <= 15; ++i) {
            add("Школа № " + std::to_string(i * 3), true);
        }
        for (auto const *s : SURNAMES) {
            add(std::string("ИП ") + s, true);
        }
        for (auto const *s : SURNAMES) {
            if (m_partners.size() >= 100) {
                break;
            }
            add(std::string(s) + " (частный заказ)", false);
        }
    }

    /// Установка цен трёх типов; percent > 0 - подорожание части позиций, 0 - начальные цены всех.
    void prices(std::string const &date, int percent) {
        Prices const types{m_db};
        if (!m_retail) {
            m_retail = Prices{m_db}.create_type("Розница");
            m_opt = Prices{m_db}.create_type("Опт");
        }
        std::vector<std::size_t> chosen;
        for (std::size_t i = 0; i < m_products.size(); ++i) {
            if (percent == 0 || m_rand.chance(35)) {
                chosen.push_back(i);
            }
        }
        for (auto const i : chosen) {
            auto &p = m_products[i];
            if (percent) {
                p.cost = round_price(p.cost * (100 + percent) / 100, p.unit);
                p.retail = round_price(p.retail * (100 + percent + 2) / 100, p.unit);
                p.opt = round_price(p.opt * (100 + percent) / 100, p.unit);
            }
        }
        m_docs.set_actor(m_tech);
        for (auto const &[type, field] :
             {std::pair{COST_PRICE_TYPE, &Product::cost}, std::pair{m_retail, &Product::retail}, std::pair{m_opt, &Product::opt}}) {
            Document d;
            d.type = DocType::PRICES;
            d.date = date;
            d.time = "06:00";
            d.price_type_id = type;
            d.comment = percent ? "Квартальное изменение цен" : "Начальные цены";
            for (auto const i : chosen) {
                d.lines.push_back({.price_kop = m_products[i].*field, .item_id = m_products[i].id});
            }
            post(d);
        }
    }

    std::int64_t post(Document const &d) {
        auto const id = m_docs.create(d);
        m_docs.post(id);
        ++m_stats.docs;
        return id;
    }

    void one_day(std::string const &date, bool today) {
        auto const day_no = static_cast<int>(m_day++);
        if (day_no > 0 && day_no % 91 == 0) {
            prices(date, static_cast<int>(m_rand.between(4, 8)));
        }
        m_docs.set_actor(m_seller);
        orders(date, 5);

        // поступление: позиции своего дня выпечки
        Document prod;
        prod.type = DocType::PRODUCTION;
        prod.date = date;
        prod.time = "07:00";
        for (auto const &p : m_products) {
            if ((day_no + p.id) % p.every == 0) {
                auto const qty = static_cast<std::int64_t>(p.batch) * 1000 * m_rand.between(80, 120) / 100;
                auto const rounded = p.unit == Unit::PCS ? std::max<std::int64_t>(1000, qty / 1000 * 1000) : qty / 100 * 100;
                prod.lines.push_back({.item_id = p.id, .qty = rounded});
                m_stock[p.id] += rounded;
            }
        }
        if (!prod.lines.empty()) {
            post(prod);
        }

        // около 100 документов в день: в основном розничные чеки, оптовые отгрузки - утром
        auto const sales = today ? 10 : static_cast<int>(m_rand.between(84, 92));
        for (int s = 0; s < sales; ++s) {
            sale(date, hhmm(8 * 60 + s * 600 / sales + static_cast<int>(m_rand.between(0, 5))), false);
        }
        if (today) {
            // незаконченная работа сегодняшнего дня: черновик реализации и заявка на завтра
            sale(date, "13:30", true);
            orders(shift(date, 1), 1);
            return;
        }
        // строго по времени: остатки генератора должны совпадать с сервером на каждый момент
        writeoff(date);
        if (shift(date, 1).substr(8, 2) == "01") {
            inventory(date);
        }
        for (auto r = m_rand.between(1, 3); r > 0; --r) {
            ret(date);
        }
    }

    void orders(std::string const &date, int count) {
        for (int i = 0; i < count; ++i) {
            Document o;
            o.type = DocType::ORDER;
            o.date = date;
            o.time = hhmm(8 * 60 + i * 30);
            o.partner_id = m_rand.pick(m_partners).id;
            auto const lines = m_rand.between(4, 9);
            std::vector<std::int64_t> used;
            for (int l = 0; l < lines; ++l) {
                auto const &p = m_rand.pick(m_products);
                if (std::find(used.begin(), used.end(), p.id) != used.end()) {
                    continue;
                }
                used.push_back(p.id);
                o.lines.push_back({.item_id = p.id, .qty = p.unit == Unit::PCS ? m_rand.between(5, 30) * 1000 : m_rand.between(1, 5) * 1000});
            }
            m_docs.create(o);
            ++m_stats.docs;
        }
    }

    Product const &product_of(std::int64_t id) const {
        return *std::find_if(m_products.begin(), m_products.end(), [&](auto const &p) { return p.id == id; });
    }

    void sale(std::string const &date, std::string const &time, bool draft) {
        std::vector<std::int64_t> available;
        for (auto const &[id, qty] : m_stock) {
            if (qty >= 1000) {
                available.push_back(id);
            }
        }
        if (available.empty()) {
            return;
        }
        // три из четырёх - розница (покупатели с улицы и частные заказы), остальное - опт
        auto const retail = m_rand.chance(75);
        auto const &partner = retail ? m_rand.pick(m_retail_partners) : m_rand.pick(m_wholesale_partners);
        Document d;
        d.type = DocType::SALE;
        d.date = date;
        d.time = time;
        d.partner_id = partner.id;
        d.price_type_id = partner.wholesale ? m_opt : m_retail;
        auto const lines = partner.wholesale ? m_rand.between(4, 12) : m_rand.between(1, 4);
        std::vector<std::int64_t> used;
        for (int l = 0; l < lines; ++l) {
            auto const id = m_rand.pick(available);
            if (std::find(used.begin(), used.end(), id) != used.end()) {
                continue;
            }
            used.push_back(id);
            auto const &p = product_of(id);
            auto const stock = m_stock[id];
            auto qty = p.unit == Unit::PCS
                           ? std::min<std::int64_t>(stock / 1000, partner.wholesale ? m_rand.between(5, 25) : m_rand.between(1, 3)) * 1000
                           : std::min<std::int64_t>(stock, m_rand.between(3, partner.wholesale ? 30 : 10) * 100);
            if (qty <= 0) {
                continue;
            }
            auto price = partner.wholesale ? p.opt : p.retail;
            // продавец иногда отпускает не по типовой цене: скидка или наценка за срочность
            if (m_rand.chance(8)) {
                price = round_price(price * m_rand.between(85, 112) / 100, p.unit);
            }
            d.lines.push_back({.price_kop = price, .item_id = id, .qty = qty});
            if (!draft) {
                m_stock[id] -= qty;
            }
        }
        if (d.lines.empty()) {
            return;
        }
        if (draft) {
            m_docs.create(d);
            ++m_stats.docs;
            return;
        }
        m_recent_sales.push_back({post(d), d});
        if (m_recent_sales.size() > 30) {
            m_recent_sales.erase(m_recent_sales.begin());
        }
    }

    void ret(std::string const &date) {
        if (m_recent_sales.empty()) {
            return;
        }
        auto const &[sale_id, sale] =
            m_recent_sales[static_cast<std::size_t>(m_rand.between(0, static_cast<std::int64_t>(m_recent_sales.size()) - 1))];
        if (m_returned.contains(sale_id)) {
            return;
        }
        auto const &line = sale.lines.front();
        auto const qty = product_of(line.item_id).unit == Unit::PCS ? std::min<std::int64_t>(line.qty, 2000) : std::min<std::int64_t>(line.qty, 300);
        Document d;
        d.type = DocType::RETURN;
        d.date = date;
        d.time = "22:30";
        d.base_id = sale_id;
        d.comment = m_rand.pick(std::vector<char const *>{"не подошёл товар", "лишнее в заказе", "заказ отменён"});
        d.lines.push_back({.item_id = line.item_id, .qty = qty});
        post(d);
        m_returned[sale_id] = true;
        m_stock[line.item_id] += qty;
    }

    void writeoff(std::string const &date) {
        Document d;
        d.type = DocType::WRITEOFF;
        d.date = date;
        d.time = "21:00";
        d.comment = m_rand.pick(std::vector<char const *>(std::begin(WRITEOFF_REASONS), std::end(WRITEOFF_REASONS)));
        // часть залежавшегося списывается: так склад не копится весь год
        for (auto &[id, qty] : m_stock) {
            if (qty > 0 && m_rand.chance(product_of(id).every == 1 ? 20 : 8)) {
                auto const unit = product_of(id).unit;
                auto const off = unit == Unit::PCS ? std::max<std::int64_t>(1000, qty / 2 / 1000 * 1000)
                                                   : std::max<std::int64_t>(100, qty / 2 / 100 * 100);
                auto const amount = std::min(off, qty);
                d.lines.push_back({.item_id = id, .qty = amount});
                qty -= amount;
            }
        }
        if (!d.lines.empty()) {
            post(d);
        }
    }

    /// Инвентаризация в последний день месяца по одной из групп продукции; недостача у части позиций.
    void inventory(std::string const &date) {
        // группа, где есть остатки: начиная со случайной, по кругу
        auto const n = static_cast<std::int64_t>(m_category_group.size());
        auto const first = m_rand.between(0, n - 1);
        for (std::int64_t k = 0; k < n; ++k) {
            if (inventory_group(date, m_category_group[static_cast<std::size_t>((first + k) % n)])) {
                return;
            }
        }
    }

    /// Инвентаризация группы gid; false, если остатков в ней нет.
    bool inventory_group(std::string const &date, std::int64_t gid) {
        auto const scope = Groups{m_db}.with_descendants(gid);
        Document d;
        d.type = DocType::INVENTORY;
        d.date = date;
        d.time = "22:00";
        d.group_id = gid;
        d.comment = "Плановая инвентаризация";
        for (auto const &[id, qty] : m_stock) {
            if (qty <= 0 || !scope.contains(m_catalog.get(id).group_id.value_or(0))) {
                continue;
            }
            auto counted = qty;
            if (m_rand.chance(15)) {
                counted -= product_of(id).unit == Unit::PCS ? 1000 : 100;
                counted = std::max<std::int64_t>(counted, 0);
            }
            d.lines.push_back({.item_id = id, .qty = counted});
            m_stock[id] = counted;
        }
        if (d.lines.empty()) {
            return false;
        }
        m_docs.set_actor(m_tech);
        post(d);
        m_docs.set_actor(m_seller);
        return true;
    }

    struct PartnerRef {
        std::int64_t id;
        bool wholesale;
    };

    std::map<std::string, std::int64_t> m_raw;
    std::map<std::string, std::int64_t> m_semi;
    std::map<std::string, std::int64_t> m_groups;
    std::map<std::int64_t, std::int64_t> m_stock; ///< остатки продукции, тысячные
    std::map<std::int64_t, bool> m_returned;      ///< реализации, по которым уже был возврат
    std::vector<std::pair<std::int64_t, Document>> m_recent_sales;
    std::vector<Product> m_products;
    std::vector<PartnerRef> m_partners;
    std::vector<PartnerRef> m_retail_partners;
    std::vector<PartnerRef> m_wholesale_partners;
    std::vector<std::int64_t> m_category_group;
    DemoStats m_stats;
    Random m_rand;
    Database &m_db;
    Catalog m_catalog{m_db};
    Documents m_docs;
    std::string m_today;
    std::size_t m_day = 0;
    int m_days;
    std::int64_t m_tech{};
    std::int64_t m_seller{};
    std::int64_t m_retail{};
    std::int64_t m_opt{};
};

} // namespace

DemoStats fill_demo(Database &db, std::string const &today, int days) {
    check_date(today);
    auto const empty = [&](char const *sql) {
        auto stmt = db.prepare(sql);
        stmt.step();
        return stmt.column_int(0) == 0;
    };
    if (!empty("SELECT count(*) FROM users") || !empty("SELECT count(*) FROM items") || !empty("SELECT count(*) FROM docs") ||
        !empty("SELECT count(*) FROM partners") || !empty("SELECT count(*) FROM groups")) {
        throw UserError("демонстрационную базу можно создать только в пустой базе");
    }
    Transaction tx{db};
    auto const stats = Demo{db, today, days}.run();
    tx.commit();
    return stats;
}

bool is_demo(Database &db) {
    auto stmt = db.prepare("SELECT value FROM settings WHERE key = 'demo'");
    return stmt.step() && stmt.column_text(0) == "1";
}
