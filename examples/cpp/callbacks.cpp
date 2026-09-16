#include <Ivy/ivy.hpp>

#include <chrono>
#include <iostream>

using namespace std::chrono_literals;

int main(int argc, char** argv) {
    auto created = ivy::Bus::create("cpp-callbacks", "cpp-callbacks ready");
    if (!created) {
        std::cerr << created.error().message() << '\n';
        return 1;
    }
    auto& bus = *created;
    // This bus advertises TICK/STATUS messages; filters concern remote subscriptions.
    if (auto configured = bus.set_filters("TICK", "STATUS"); !configured) {
        std::cerr << configured.error().message() << '\n';
        return 1;
    }

    auto pongs = bus.bind_event([](IvyClientPtr, int delay_us) {
        if (delay_us < 0)
            std::cout << "Ping timed out: " << delay_us << " us\n";
        else
            std::cout << "Pong received in " << delay_us << " us\n";
    }, ivy::pong);

    auto changes = bus.bind_event([&bus, ping_sent = false]
        (IvyClientPtr peer, int id, std::string_view regexp, IvyBindEvent event) mutable {
            std::cout << "Remote subscription " << id << ": " << regexp
                      << " (event " << event << ")\n";
            // The event supplies a connected peer belonging to this bus.
            if (event == IvyAddBind && !ping_sent) {
                if (auto sent = bus.send_ping(peer); !sent)
                    std::cerr << sent.error().message() << '\n';
                else
                    ping_sent = true;
            }
        }, ivy::remote_bindings);

    auto timer = bus.bind_event([&bus, ticks = 0U](std::chrono::milliseconds late) mutable {
        const auto report = bus.send_report("TICK {}", ++ticks);
        std::cout << "Tick " << ticks << ", " << late.count() << " ms late, "
                  << report.accepted << '/' << report.matched << " frames accepted\n";
        if (report.error) {
            std::cerr << report.error.message() << ": " << report.failed << " failed\n";
            if (report.system_error)
                std::cerr << report.system_error.message() << '\n';
        }
    }, ivy::every(1s));

    if (!pongs || !changes || !timer) {
        const auto error = !pongs ? pongs.error() : !changes ? changes.error() : timer.error();
        std::cerr << error.message() << '\n';
        return 1;
    }
    if (auto changed = timer->set_period(500ms); !changed) {
        std::cerr << changed.error().message() << '\n';
        return 1;
    }
    const auto started = argc > 1 ? bus.start(argv[1]) : bus.start();
    if (!started) {
        std::cerr << started.error().message() << '\n';
        return 1;
    }

    // Keep all three subscriptions alive until another application sends a die request.
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
