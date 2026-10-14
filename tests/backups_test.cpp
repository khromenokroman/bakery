#include "backups.hpp"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

#include "errors.hpp"
#include "fixture.hpp"
#include "test_util.hpp"

namespace fs = std::filesystem;

namespace {

/// Каталог копий во временном каталоге, удаляется после теста.
class BackupsTest : public DbFixture {
   protected:
    BackupsTest()
        : m_dir{fs::temp_directory_path() / ("bakery-backups-" + std::to_string(::testing::UnitTest::GetInstance()->random_seed()) + "-" +
                                             ::testing::UnitTest::GetInstance()->current_test_info()->name())},
          m_backups{m_db, m_dir} {
        fs::remove_all(m_dir);
    }
    ~BackupsTest() override { fs::remove_all(m_dir); }

    std::vector<std::string> names() const {
        std::vector<std::string> out;
        for (auto const &b : m_backups.list()) {
            out.push_back(b.name);
        }
        return out;
    }

    fs::path m_dir;
    Backups m_backups;
};

} // namespace

/**
 * @brief Ручная копия - целая база с данными; список новые первыми; удаление; чужие имена и выход из каталога
 * отклоняются как несуществующие копии.
 */
TEST_F(BackupsTest, CreateListRemove) {
    add("Батон", Kind::PRODUCT, Unit::PCS);
    auto const first = m_backups.create(false, "2026-10-14 09:30:05");
    EXPECT_EQ(first.name, "bakery-20261014-093005-manual.db");
    EXPECT_EQ(first.created, "2026-10-14 09:30:05");
    EXPECT_FALSE(first.scheduled);
    EXPECT_GT(first.size, 0U) << "размер копии";
    {
        Database copy{(m_dir / first.name).string()};
        auto stmt = copy.prepare("SELECT name FROM items");
        ASSERT_TRUE(stmt.step()) << "в копии должна быть номенклатура";
        EXPECT_EQ(stmt.column_text(0), "Батон");
    }
    EXPECT_THROW_REASON(m_backups.create(false, "2026-10-14 09:30:05"), UserError, "уже есть");
    m_backups.create(true, "2026-10-15 03:00:00");
    std::ofstream{m_dir / "notes.txt"} << "чужой файл";
    EXPECT_THAT(names(), testing::ElementsAre("bakery-20261015-030000-auto.db", "bakery-20261014-093005-manual.db"))
        << "новые первыми, чужие файлы не показываются";
    EXPECT_FALSE(fs::exists(m_dir / (first.name + ".tmp"))) << "временный файл не должен оставаться";

    EXPECT_THROW((void)m_backups.path("../bakery.db"), NotFoundError);
    EXPECT_THROW((void)m_backups.path("notes.txt"), NotFoundError);
    EXPECT_THROW(m_backups.remove("bakery-20200101-000000-manual.db"), NotFoundError);
    m_backups.remove(first.name);
    EXPECT_THAT(names(), testing::ElementsAre("bakery-20261015-030000-auto.db"));
}

/**
 * @brief Расписание: по умолчанию выключено; время и число копий проверяются.
 */
TEST_F(BackupsTest, Schedule) {
    auto s = m_backups.schedule();
    EXPECT_FALSE(s.enabled) << "по умолчанию расписание выключено";
    EXPECT_EQ(s.time, "03:00");
    EXPECT_EQ(s.keep, 14);
    s.enabled = true;
    s.time = "2:30";
    EXPECT_THROW_REASON(m_backups.set_schedule(s), UserError, "неверное время");
    s.time = "02:30";
    s.keep = 0;
    EXPECT_THROW_REASON(m_backups.set_schedule(s), UserError, "от 1 до 365");
    s.keep = 3;
    m_backups.set_schedule(s);
    s = m_backups.schedule();
    EXPECT_TRUE(s.enabled);
    EXPECT_EQ(s.time, "02:30");
    EXPECT_EQ(s.keep, 3);
}

/**
 * @brief Копия по расписанию раз в день после заданного времени; хранятся последние keep копий по расписанию,
 * ручные не удаляются.
 */
TEST_F(BackupsTest, RunDueAndPrune) {
    EXPECT_FALSE(m_backups.run_due("2026-10-14 04:00:00")) << "расписание выключено";
    m_backups.set_schedule({.time = "03:00", .keep = 2, .enabled = true});
    EXPECT_FALSE(m_backups.run_due("2026-10-14 02:59:59")) << "время ещё не наступило";
    auto const done = m_backups.run_due("2026-10-14 03:00:30");
    ASSERT_TRUE(done) << "в 03:00 копия делается";
    EXPECT_TRUE(done->scheduled);
    EXPECT_FALSE(m_backups.run_due("2026-10-14 15:00:00")) << "второй раз за день не делается";
    m_backups.create(false, "2026-10-14 16:00:00");
    // сервер был выключен ночью: копия делается при первой проверке после времени
    ASSERT_TRUE(m_backups.run_due("2026-10-15 09:12:00"));
    ASSERT_TRUE(m_backups.run_due("2026-10-16 03:00:00"));
    EXPECT_THAT(names(), testing::ElementsAre("bakery-20261016-030000-auto.db", "bakery-20261015-091200-auto.db", "bakery-20261014-160000-manual.db"))
        << "хранятся 2 последние копии по расписанию и ручная";
    EXPECT_EQ(m_backups.schedule().last_run, "2026-10-16");
    EXPECT_EQ(m_backups.schedule().last_error, "");
}

/**
 * @brief Ошибка копии по расписанию запоминается, и повтор не идёт до следующего дня.
 */
TEST_F(BackupsTest, RunDueError) {
    std::ofstream{m_dir} << "файл на месте каталога";
    m_backups.set_schedule({.time = "03:00", .keep = 2, .enabled = true});
    EXPECT_THROW_REASON(m_backups.run_due("2026-10-14 03:00:00"), UserError, "каталог копий");
    EXPECT_THAT(m_backups.schedule().last_error, testing::HasSubstr("каталог копий")) << "ошибка видна в расписании";
    EXPECT_FALSE(m_backups.run_due("2026-10-14 03:00:30")) << "в тот же день повтора нет";
}
