#include <Ivy/ivy.hpp>

#include <iostream>
#include <memory>
#include <string>

int main(int argc, char** argv) {
    try {
        ivy::Bus bus("cpp-example", "cpp-example ready",
            [connections = std::make_unique<unsigned>(0)]
            (IvyClientPtr, IvyApplicationEvent event) {
                if (event == IvyApplicationConnected)
                    std::cout << "Connection " << ++*connections << '\n';
            },
            [](IvyClientPtr, int id) {
                std::cout << "Termination requested, id=" << id << '\n';
            });

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
        auto anywhere = bus.bind(
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
        if (auto changed = messages->change(std::string_view(dynamic_pattern)); !changed) {
            std::cerr << "Unable to change subscription: " << changed.error().message() << '\n';
            return 1;
        }

        const auto started = argc > 1 ? bus.start(argv[1]) : bus.start();
        if (!started) {
            std::cerr << "Unable to start Ivy: " << started.error().message() << '\n';
            return 1;
        }

        // The loop API will be designed separately. A received die request
        // stops this C loop; the bus is destroyed after the loop returns.
        IvyContextMainLoop(bus.native_handle());
        bus.rethrow_callback_exception();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
