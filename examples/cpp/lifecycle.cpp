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
