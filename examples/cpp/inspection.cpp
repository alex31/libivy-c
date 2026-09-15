#include <Ivy/ivy.hpp>

#include <chrono>
#include <iostream>

using namespace std::chrono_literals;

int main(int argc, char** argv) {
    auto created = ivy::Bus::create("cpp-inspection");
    if (!created) {
        std::cerr << created.error().message() << '\n';
        return 1;
    }
    auto& bus = *created;
    auto inspect = [&bus](std::chrono::milliseconds) {
        const auto names = bus.applications();
        if (!names) {
            std::cerr << names.error().message() << '\n';
            return;
        }
        std::cout << names->size() << " connected applications\n";
        for (const auto& name : *names) {
            const auto found = bus.find_application(name);
            if (!found) {
                std::cerr << found.error().message() << '\n';
                continue;
            }
            if (!*found) continue; // A peer may disconnect between two queries.
            const auto info = bus.application(**found);
            if (!info) {
                std::cerr << info.error().message() << '\n';
                continue;
            }
            const auto& [application_name, host] = *info;
            std::cout << application_name << " on " << host << '\n';
            const auto regexps = bus.application_regexps(**found);
            if (!regexps) {
                std::cerr << regexps.error().message() << '\n';
                continue;
            }
            for (const auto& regexp : *regexps)
                std::cout << "  " << regexp << '\n';
        }
    };

    auto once = bus.bind(inspect, ivy::after(250ms));
    auto repeated = bus.bind(inspect, ivy::every(1s, 3));
    auto finish = bus.bind([&bus](std::chrono::milliseconds) {
        if (auto stopped = bus.stop(); !stopped)
            std::cerr << stopped.error().message() << '\n';
    }, ivy::after(4s));
    if (!once || !repeated || !finish) {
        const auto error = !once ? once.error() : !repeated ? repeated.error() : finish.error();
        std::cerr << error.message() << '\n';
        return 1;
    }
    const auto started = argc > 1 ? bus.start(argv[1]) : bus.start();
    if (!started) {
        std::cerr << started.error().message() << '\n';
        return 1;
    }
    if (auto result = bus.run(); !result) {
        std::cerr << result.error().message() << '\n';
        return 1;
    }
    if (auto callbacks = bus.take_callback_error(); !callbacks) {
        std::cerr << callbacks.error().message() << '\n';
        return 1;
    }
    return 0;
}
