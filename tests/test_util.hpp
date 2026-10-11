#pragma once
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <string>

/**
 * @brief Ожидает, что statement бросит исключение type с текстом, содержащим reason.
 *
 * При падении выводит сам оператор, ожидаемую причину и фактический текст или тип исключения.
 */
#define EXPECT_THROW_REASON(statement, type, reason)                                                                 \
    do {                                                                                                             \
        try {                                                                                                        \
            statement;                                                                                               \
            ADD_FAILURE() << "ожидался отказ " #type " с причиной \"" << (reason) << "\"\n  в: " #statement;         \
        } catch (type const &e) {                                                                                    \
            EXPECT_THAT(std::string(e.what()), testing::HasSubstr(reason)) << "в: " #statement;                      \
        } catch (std::exception const &e) {                                                                          \
            ADD_FAILURE() << "ожидался " #type ", получено другое исключение: " << e.what() << "\n  в: " #statement; \
        }                                                                                                            \
    } while (false)
