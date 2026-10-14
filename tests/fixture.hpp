#pragma once
#include <gtest/gtest.h>

#include <string>
#include <utility>
#include <vector>

#include "catalog.hpp"
#include "db.hpp"
#include "docs.hpp"
#include "partners.hpp"
#include "schema.hpp"

/**
 * @brief Пустая база в памяти со справочником и помощниками для заполнения.
 */
class DbFixture : public testing::Test {
   protected:
    DbFixture() : m_db{":memory:"}, m_catalog{m_db}, m_docs{m_db} {
        migrate(m_db);
        Partner buyer;
        buyer.name = "Покупатель";
        m_buyer = Partners{m_db}.create(buyer);
    }

    std::int64_t add(std::string const &name, Kind kind, Unit unit = Unit::KG) {
        Item item;
        item.name = name;
        item.kind = kind;
        item.unit = unit;
        return m_catalog.create(item);
    }

    /// Рецептура из пар {компонент, количество в тысячных}.
    static Recipe make_recipe(std::int64_t output_qty, std::vector<std::pair<std::int64_t, std::int64_t>> const &lines) {
        Recipe r;
        r.output_qty = output_qty;
        for (auto const &[id, qty] : lines) {
            RecipeLine l;
            l.component_id = id;
            l.qty = qty;
            r.lines.push_back(l);
        }
        return r;
    }

    /// Документ вида type из пар {продукция, количество в тысячных}; у реализации клиент m_buyer.
    Document make_doc(DocType type, std::string date, std::vector<std::pair<std::int64_t, std::int64_t>> const &lines,
                      std::string comment = {}) const {
        Document d;
        d.type = type;
        d.date = std::move(date);
        d.comment = std::move(comment);
        if (type == DocType::SALE) {
            d.partner_id = m_buyer;
        }
        for (auto const &[id, qty] : lines) {
            DocLine l;
            l.item_id = id;
            l.qty = qty;
            d.lines.push_back(l);
        }
        return d;
    }

    /// Заявка из пар {продукция, количество в тысячных}.
    Document make_order(std::string date, std::vector<std::pair<std::int64_t, std::int64_t>> const &lines, std::string comment = {}) const {
        return make_doc(DocType::ORDER, std::move(date), lines, std::move(comment));
    }

    Database m_db;
    Catalog m_catalog;
    Documents m_docs;
    std::int64_t m_buyer{}; ///< клиент для реализаций
};
