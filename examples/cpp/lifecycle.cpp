#include <Ivy/ivy.hpp>

#include <iostream>
#include <string>

int main(int argc, char** argv) {
    auto created = ivy::Bus::create("cpp-example", "cpp-example ready",
        [connections = 0U]
        (IvyClientPtr, IvyApplicationEvent event) mutable {
            if (event == IvyApplicationConnected)
                std::cout << "Connection " << ++connections << '\n';
        },
        [](IvyClientPtr, int id) {
            std::cout << "Termination requested, id=" << id << '\n';
        });

    if (!created) {
        std::cerr << "Unable to create Ivy: " << created.error().message() << '\n';
        return 1;
    }
    auto& bus = *created;

    // Keep the Subscription and DirectSubscription results alive.
    auto messages = bus.bind(
        [](IvyClientPtr, std::span<const std::string_view> args) {
            if (!args.empty())
                std::cout << "Greeting: " << args[0] << '\n';
        }, R"(^HELLO (.*)$)");
    auto tracks = bus.bind(
        [](IvyClientPtr, std::span<const std::string_view> args) {
            if (!args.empty())
                std::cout << "TRACK 42: " << args[0] << '\n';
        }, R"(^TRACK {} ([0-9]{{2}})$)", 42);
    auto direct = bus.bind(
        [](IvyClientPtr, int id, std::string_view message) {
            std::cout << "Direct " << id << ": " << message << '\n';
        });
    auto anywhere = bus.bind_unanchored(
        [](IvyClientPtr, std::span<const std::string_view> args) {
            if (!args.empty())
                std::cout << "Unanchored alert: " << args[0] << '\n';
        }, R"(ALERT (.*)$)");
    if (!messages || !tracks || !direct || !anywhere) {
        const auto error = !messages ? messages.error() : !tracks ? tracks.error()
            : !direct ? direct.error() : anywhere.error();
        std::cerr << "Unable to subscribe: " << error.message() << '\n';
        return 1;
    }

    // Only regexp subscriptions offer change(); the callback stays the same.
    const std::string dynamic_pattern = R"(^BONJOUR (.*)$)";
    if (auto changed = messages->change(ivy::runtime_regexp(dynamic_pattern)); !changed) {
        std::cerr << "Unable to change subscription: " << changed.error().message() << '\n';
        return 1;
    }

    if (auto configured = bus.set_transport_error_callback(
            [](IvyClientPtr, std::error_code error, int system_error) {
                std::cerr << "Transport: " << error.message()
                          << " (OS " << system_error << ")\n";
            }); !configured) {
        std::cerr << configured.error().message() << '\n';
        return 1;
    }

    const auto started = argc > 1 ? bus.start(argv[1]) : bus.start();
    if (!started) {
        std::cerr << "Unable to start Ivy: " << started.error().message() << '\n';
        return 1;
    }

    // Zero accepted frames is normal if no matching peers are known yet.
    const auto report = bus.send_report("HELLO {}", "from C++23");
    if (report.error) {
        std::cerr << "Unable to send: " << report.error.message()
                  << " (" << report.accepted << " accepted, " << report.failed << " failed)\n";
        if (report.system_error)
            std::cerr << report.system_error.message() << '\n';
    }

    // A received die request stops the loop; destruction follows its return.
    IvyContextMainLoop(bus.native_handle());
    if (auto callbacks = bus.take_callback_error(); !callbacks) {
        std::cerr << "Callback failed: " << callbacks.error().message() << '\n';
        return 1;
    }
    return 0;
}
