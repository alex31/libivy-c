#include <Ivy/ivy.hpp>

#include <cstdlib>
#include <getopt.h>
#include <iostream>
#include <span>
#include <string_view>

int main(int argc, char** argv) {
    const char* bus = nullptr;
    int c;
    while ((c = getopt(argc, argv, "b:")) != -1) {
        switch (c) {
        case 'b':
            bus = optarg;
            break;
        default:
            break;
        }
    }

    if (!bus && optind < argc) {
        bus = argv[optind];
    }
    if (!bus) {
        bus = std::getenv("IVYBUS");
    }

    // Initialize the Ivy bus with the application name and ready message
    auto created = ivy::Bus::create("IvyTranslater", "Hello le monde");
    if (!created) {
        std::cerr << "IvyContextCreate failed: " << created.error().message() << '\n';
        return 1;
    }
    auto& translater_bus = *created;

    // Binding of callback to messages starting with 'Hello'
    auto hello_sub = translater_bus.bind_raw([&translater_bus](std::span<const std::string_view> args) {
        const std::string_view arg = args.empty() ? "" : args[0];
        if (auto sent = translater_bus.send("Bonjour{}", arg); !sent) {
            std::cerr << "Ivy send failed: " << sent.error().message() << '\n';
        }
    }, R"(^Hello(.*))");

    // Binding of callback to 'Bye'
    auto bye_sub = translater_bus.bind_raw([&translater_bus](std::span<const std::string_view>) {
        if (auto stopped = translater_bus.stop(); !stopped) {
            std::cerr << "IvyContextStop failed: " << stopped.error().message() << '\n';
        }
    }, R"(^Bye$)");

    if (!hello_sub || !bye_sub) {
        const auto error = !hello_sub ? hello_sub.error() : bye_sub.error();
        std::cerr << "Ivy subscription failed: " << error.message() << '\n';
        return 1;
    }

    // Start Ivy bus connection
    const auto started = bus ? translater_bus.start(bus) : translater_bus.start();
    if (!started) {
        std::cerr << "IvyContextStart failed: " << started.error().message() << '\n';
        return 1;
    }

    // Run the main event loop
    if (auto result = translater_bus.run(); !result) {
        std::cerr << "IvyContextMainLoop failed: " << result.error().message() << '\n';
        return 1;
    }

    if (auto callbacks = translater_bus.take_callback_error(); !callbacks) {
        std::cerr << "Ivy callback failed: " << callbacks.error().message() << '\n';
        return 1;
    }

    return 0;
}
