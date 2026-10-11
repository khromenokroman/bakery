#include <httplib.h>
#include <pthread.h>
#include <signal.h>
#include <unistd.h>

#include <cstdlib>
#include <exception>
#include <iostream>
#include <string_view>
#include <thread>
#include <vector>

#include "api.hpp"
#include "db.hpp"
#include "options.hpp"
#include "schema.hpp"

namespace {

constexpr int EXIT_USAGE = 2;

} // namespace

int main(int argc, char *argv[]) {
    Options opt;
    try {
        opt = parse_options(std::vector<std::string_view>(argv + 1, argv + argc));
    } catch (std::exception const &e) {
        std::cerr << "bakery: " << e.what() << "\n" << usage();
        return EXIT_USAGE;
    }
    if (opt.help) {
        std::cout << usage();
        return EXIT_SUCCESS;
    }
    if (opt.version) {
        std::cout << "bakery " << BAKERY_VERSION << "\n";
        return EXIT_SUCCESS;
    }

    try {
        Database db{opt.db_path};
        auto const version = migrate(db);

        // SIGINT и SIGTERM принимает отдельный поток через sigwait и останавливает сервер
        sigset_t signals;
        sigemptyset(&signals);
        sigaddset(&signals, SIGINT);
        sigaddset(&signals, SIGTERM);
        pthread_sigmask(SIG_BLOCK, &signals, nullptr);

        httplib::Server server;
        Api api{db, opt.web_dir};
        api.register_routes(server);

        std::thread waiter{[&server, &signals] {
            int sig = 0;
            sigwait(&signals, &sig);
            server.stop();
        }};

        auto ok = server.bind_to_port(opt.host, opt.port);
        if (ok) {
            std::cout << "bakery " << BAKERY_VERSION << ": база " << opt.db_path << " (схема " << version << "), http://" << opt.host << ":"
                      << opt.port << "/" << std::endl;
            ok = server.listen_after_bind();
        } else {
            std::cerr << "bakery: не удалось занять " << opt.host << ":" << opt.port << "\n";
        }
        // будим поток ожидания, если сервер завершился не по сигналу
        kill(getpid(), SIGTERM);
        waiter.join();
        return ok ? EXIT_SUCCESS : EXIT_FAILURE;
    } catch (std::exception const &e) {
        std::cerr << "bakery: " << e.what() << "\n";
        return EXIT_FAILURE;
    }
}
