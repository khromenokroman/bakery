#include "requirements.hpp"

#include <algorithm>
#include <map>
#include <optional>
#include <set>

#include "docs.hpp"
#include "errors.hpp"

namespace {

/// ceil(a * b / c) без переполнения промежуточного произведения; a, b >= 0, c > 0.
std::int64_t mul_div_ceil(std::int64_t a, std::int64_t b, std::int64_t c) {
    auto const p = static_cast<__int128>(a) * b;
    auto const q = (p + c - 1) / c;
    if (q > INT64_MAX) {
        throw UserError("потребность слишком велика для расчёта");
    }
    return static_cast<std::int64_t>(q);
}

/**
 * @brief Позиции и рецептуры, нужные для расчёта, загруженные один раз.
 */
class Graph {
   public:
    explicit Graph(Database &db) : m_catalog{db} {}

    Item const &item(std::int64_t id) {
        auto it = m_items.find(id);
        if (it == m_items.end()) {
            it = m_items.emplace(id, m_catalog.get(id)).first;
        }
        return it->second;
    }

    std::optional<Recipe> const &recipe(std::int64_t id) {
        auto it = m_recipes.find(id);
        if (it == m_recipes.end()) {
            it = m_recipes.emplace(id, m_catalog.recipe(id)).first;
        }
        return it->second;
    }

    /// Обратный топологический порядок обхода от id (потомки раньше предков) в out.
    void post_order(std::int64_t id, std::set<std::int64_t> &visited, std::vector<std::int64_t> &out) {
        if (!visited.insert(id).second) {
            return;
        }
        if (auto const &r = recipe(id)) {
            for (auto const &line : r->lines) {
                post_order(line.component_id, visited, out);
            }
        }
        out.push_back(id);
    }

   private:
    std::map<std::int64_t, Item> m_items;
    std::map<std::int64_t, std::optional<Recipe>> m_recipes;
    Catalog m_catalog;
};

} // namespace

Requirements compute_requirements(Database &db, std::vector<std::int64_t> const &doc_ids) {
    if (doc_ids.empty()) {
        throw UserError("не выбрано ни одной заявки");
    }
    Documents const docs{db};
    std::map<std::int64_t, std::int64_t> ordered;
    for (auto const id : std::set<std::int64_t>(doc_ids.begin(), doc_ids.end())) {
        auto const doc = docs.get(id);
        if (doc.type != DocType::ORDER) {
            throw UserError("документ " + std::to_string(id) + " не заявка");
        }
        for (auto const &line : doc.lines) {
            ordered[line.item_id] += line.qty;
        }
    }

    Graph graph{db};
    std::set<std::int64_t> visited;
    std::vector<std::int64_t> order;
    for (auto const &[id, qty] : ordered) {
        graph.post_order(id, visited, order);
    }
    std::reverse(order.begin(), order.end());

    // спрос из заявок плюс спрос из рецептур уже развёрнутых предков
    std::map<std::int64_t, std::int64_t> demand = ordered;
    std::map<std::int64_t, std::int64_t> from_recipes;
    for (auto const id : order) {
        auto const total = demand[id];
        auto const &r = graph.recipe(id);
        if (!r || total == 0) {
            continue;
        }
        for (auto const &line : r->lines) {
            auto const need = mul_div_ceil(total, line.qty, r->output_qty);
            demand[line.component_id] += need;
            from_recipes[line.component_id] += need;
        }
    }

    Requirements result;
    auto const make = [&](std::int64_t id, std::int64_t qty) {
        auto const &item = graph.item(id);
        return Need{item.name, id, qty, item.kind, item.unit, graph.recipe(id).has_value()};
    };
    for (auto const &[id, qty] : ordered) {
        result.products.push_back(make(id, qty));
    }
    for (auto const id : order) {
        auto const &item = graph.item(id);
        if (item.kind == Kind::RAW) {
            // штучное сырьё (яйца) докупается целыми штуками
            auto const qty = item.unit == Unit::PCS ? mul_div_ceil(demand[id], 1, 1000) * 1000 : demand[id];
            result.raw.push_back(make(id, qty));
        } else if (from_recipes.contains(id)) {
            result.intermediates.push_back(make(id, from_recipes[id]));
        }
        if (item.kind != Kind::RAW && !graph.recipe(id)) {
            result.warnings.push_back("«" + item.name + "»: нет рецептуры, сырьё для неё не рассчитано");
        }
    }

    auto const by_name = [](Need const &a, Need const &b) { return a.name < b.name; };
    std::sort(result.products.begin(), result.products.end(), by_name);
    std::sort(result.intermediates.begin(), result.intermediates.end(), by_name);
    std::sort(result.raw.begin(), result.raw.end(), by_name);
    std::sort(result.warnings.begin(), result.warnings.end());
    return result;
}
