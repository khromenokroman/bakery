cmake_minimum_required(VERSION 3.25.1)
project(bakery
        VERSION 0.7.0.0
        LANGUAGES CXX
        HOMEPAGE_URL "https://github.com/khromenokroman/bakery")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(BUILD_TESTS "Собирать тесты" ON)

find_package(SQLite3 REQUIRED)
find_package(nlohmann_json 3 REQUIRED)
find_package(Threads REQUIRED)

include(GNUInstallDirs)

configure_file(Version.in Version @ONLY)

# cpp-httplib header-only из third_party: пакет Debian собран в разделяемую библиотеку без .a
add_library(httplib INTERFACE)
target_include_directories(httplib SYSTEM INTERFACE third_party)

set(BAKERY_WEB_DIR "${CMAKE_INSTALL_FULL_DATADIR}/${PROJECT_NAME}/web")

add_library(${PROJECT_NAME}-core STATIC
        src/api.cpp
        src/catalog.cpp
        src/db.cpp
        src/docs.cpp
        src/groups.cpp
        src/json_io.cpp
        src/options.cpp
        src/prices.cpp
        src/requirements.cpp
        src/schema.cpp
        src/stock.cpp
)
target_include_directories(${PROJECT_NAME}-core PUBLIC src)
target_compile_definitions(${PROJECT_NAME}-core PUBLIC
        BAKERY_VERSION="${PROJECT_VERSION}"
        BAKERY_WEB_DIR="${BAKERY_WEB_DIR}")
target_link_libraries(${PROJECT_NAME}-core PUBLIC
        httplib SQLite::SQLite3 nlohmann_json::nlohmann_json Threads::Threads)

add_executable(${PROJECT_NAME}
        src/main.cpp
)
target_link_libraries(${PROJECT_NAME} PRIVATE ${PROJECT_NAME}-core)

if (BUILD_TESTS)
    enable_testing()
    find_package(GTest REQUIRED)
    add_executable(${PROJECT_NAME}-tests
            tests/api_test.cpp
            tests/catalog_test.cpp
            tests/docs_test.cpp
            tests/groups_test.cpp
            tests/options_test.cpp
            tests/prices_test.cpp
            tests/requirements_test.cpp
            tests/schema_test.cpp
            tests/stock_test.cpp
    )
    target_link_libraries(${PROJECT_NAME}-tests PRIVATE ${PROJECT_NAME}-core GTest::gtest_main GTest::gmock)
    include(GoogleTest)
    gtest_discover_tests(${PROJECT_NAME}-tests)
endif ()

install(TARGETS ${PROJECT_NAME}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
install(DIRECTORY web/
        DESTINATION ${BAKERY_WEB_DIR}
)
install(FILES debian/copyright
        DESTINATION ${CMAKE_INSTALL_DOCDIR}
)

set(CPACK_PACKAGE_NAME "${PROJECT_NAME}")
set(CPACK_PACKAGE_VENDOR "khromenokroman")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_CONTACT "khromenokroman <roma55592@yandex.ru>")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Bakery production and sales accounting with web interface")
set(CPACK_PACKAGE_DESCRIPTION "Products with multi-level recipes, production orders with raw material\nrequirements, finished goods stock: production receipts, sales, returns,\nwrite-offs and inventory.")
set(CPACK_PACKAGE_HOMEPAGE_URL "${PROJECT_HOMEPAGE_URL}")

set(CPACK_GENERATOR "DEB")
set(CPACK_STRIP_FILES ON)
set(CPACK_DEBIAN_PACKAGE_MAINTAINER "${CPACK_PACKAGE_CONTACT}")
set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
set(CPACK_DEBIAN_PACKAGE_SECTION "misc")
set(CPACK_DEBIAN_PACKAGE_PRIORITY "optional")
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
set(CPACK_INSTALL_DEFAULT_DIRECTORY_PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)

include(CPack)
