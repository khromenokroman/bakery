#include "json_io.hpp"

#include "errors.hpp"

namespace {

nlohmann::json const &field(nlohmann::json const &j, std::string_view key) {
    auto const it = j.find(key);
    if (it == j.end()) {
        throw UserError("нет поля " + std::string(key));
    }
    return *it;
}

} // namespace

nlohmann::json parse_object(std::string const &body) {
    auto j = nlohmann::json::parse(body, nullptr, false);
    if (j.is_discarded()) {
        throw UserError("тело запроса не является JSON");
    }
    if (!j.is_object()) {
        throw UserError("тело запроса должно быть JSON-объектом");
    }
    return j;
}

std::string get_string(nlohmann::json const &j, std::string_view key) {
    auto const &v = field(j, key);
    if (!v.is_string()) {
        throw UserError("поле " + std::string(key) + " должно быть строкой");
    }
    return v.get<std::string>();
}

std::int64_t get_int(nlohmann::json const &j, std::string_view key) {
    auto const &v = field(j, key);
    if (!v.is_number_integer()) {
        throw UserError("поле " + std::string(key) + " должно быть целым числом");
    }
    return v.get<std::int64_t>();
}

std::optional<bool> get_bool(nlohmann::json const &j, std::string_view key) {
    auto const it = j.find(key);
    if (it == j.end()) {
        return std::nullopt;
    }
    if (!it->is_boolean()) {
        throw UserError("поле " + std::string(key) + " должно быть true или false");
    }
    return it->get<bool>();
}

nlohmann::json to_json(Item const &item) {
    return {
        {"id", item.id},
        {"name", item.name},
        {"kind", to_string(item.kind)},
        {"unit", to_string(item.unit)},
        {"archived", item.archived},
        {"has_recipe", item.has_recipe},
    };
}

nlohmann::json to_json(Recipe const &recipe) {
    auto lines = nlohmann::json::array();
    for (auto const &l : recipe.lines) {
        lines.push_back({
            {"component_id", l.component_id},
            {"name", l.name},
            {"kind", to_string(l.kind)},
            {"unit", to_string(l.unit)},
            {"qty", l.qty},
        });
    }
    return {{"output_qty", recipe.output_qty}, {"lines", lines}};
}

Item item_from_json(nlohmann::json const &j, Item base) {
    if (j.contains("name")) {
        base.name = get_string(j, "name");
    }
    if (j.contains("kind")) {
        base.kind = kind_from_string(get_string(j, "kind"));
    }
    if (j.contains("unit")) {
        base.unit = unit_from_string(get_string(j, "unit"));
    }
    if (auto const archived = get_bool(j, "archived")) {
        base.archived = *archived;
    }
    return base;
}

Recipe recipe_from_json(nlohmann::json const &j) {
    Recipe recipe;
    recipe.output_qty = get_int(j, "output_qty");
    auto const &lines = field(j, "lines");
    if (!lines.is_array()) {
        throw UserError("поле lines должно быть массивом");
    }
    for (auto const &l : lines) {
        if (!l.is_object()) {
            throw UserError("строка рецептуры должна быть объектом");
        }
        RecipeLine line;
        line.component_id = get_int(l, "component_id");
        line.qty = get_int(l, "qty");
        recipe.lines.push_back(line);
    }
    return recipe;
}
