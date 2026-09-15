#include "ivy.hpp"

#include <algorithm>
#include <atomic>
#include <array>
#include <barrier>
#include <cassert>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

ivy::Bus require_bus(ivy::Bus::CreateResult result) {
    assert(result);
    return std::move(*result);
}

using namespace std::chrono_literals;

void require_start(const std::expected<void, std::error_code>& result, const char* label) {
    if (!result)
        throw std::system_error(result.error(), label);
}

template<class T>
T require_bind(std::expected<T, std::error_code> result) {
    if (!result)
        throw std::system_error(result.error(), "bind");
    return std::move(*result);
}

bool advertises(const ivy::Bus& bus, IvyClientPtr peer, std::string_view regexp) {
    std::array<char, 16384> buffer{};
    const int size = IvyContextGetApplicationMessagesBuffer(bus.native_handle(), peer,
        buffer.data(), buffer.size(), "\n");
    if (size < 0 || size > static_cast<int>(buffer.size()))
        throw std::runtime_error("query peer regexps failed");
    return std::string_view(buffer.data()).find(regexp) != std::string_view::npos;
}

// Until the C++ loop API is designed, use the borrowed C handle explicitly.
class Loop {
public:
    explicit Loop(ivy::Bus& bus)
        : context_(bus.native_handle()),
          thread_([context = context_] { IvyContextMainLoop(context); }) {}
    ~Loop() { finish(); }
    void finish() {
        if (thread_.joinable()) {
            (void)IvyContextStop(context_);
            thread_.join();
        }
    }
private:
    IvyContext* context_;
    std::thread thread_;
};

template<class Predicate>
void wait_for(Predicate predicate, const char* label) {
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (!predicate()) {
        if (std::chrono::steady_clock::now() >= deadline)
            throw std::runtime_error(label);
        std::this_thread::sleep_for(5ms);
    }
}

int main(int argc, char** argv) {
    if (argc != 3) {
        std::cerr << "usage: integration_test BUS_A BUS_B\n";
        return 2;
    }
    try {
        std::atomic<int> connected_a{0}, connected_b{0}, died_a{0}, died_b{0};
        std::atomic<int> unexpected{0};
        IvyContext* context_a = nullptr;
        IvyContext* context_b = nullptr;
        auto bus_a = require_bus(ivy::Bus::create("cpp-receiver-a", "cpp-a ready",
            [token = std::make_unique<int>(11), &connected_a, &unexpected, &context_a]
            (IvyClientPtr app, IvyApplicationEvent event) {
                if (event == IvyApplicationConnected) {
                    const char* name = IvyContextGetApplicationName(context_a, app);
                    if (*token != 11 || !name || std::string_view(name) != "cpp-peer-a")
                        ++unexpected;
                    ++connected_a;
                }
            },
            [token = std::make_unique<int>(22), &died_a, &unexpected](IvyClientPtr, int id) {
                if (*token != 22 || id != 0)
                    ++unexpected;
                ++died_a;
            }));
        auto bus_b = require_bus(ivy::Bus::create("cpp-receiver-b", std::nullopt,
            [token = std::make_unique<int>(33), &connected_b, &unexpected, &context_b]
            (IvyClientPtr app, IvyApplicationEvent event) {
                if (event == IvyApplicationConnected) {
                    const char* name = IvyContextGetApplicationName(context_b, app);
                    if (*token != 33 || !name || std::string_view(name) != "cpp-peer-b")
                        ++unexpected;
                    ++connected_b;
                }
            },
            [token = std::make_unique<int>(44), &died_b, &unexpected](IvyClientPtr, int id) {
                if (*token != 44 || id != 0)
                    ++unexpected;
                ++died_b;
                throw std::runtime_error("die callback failure");
            }));
        auto peer_a = require_bus(ivy::Bus::create("cpp-peer-a"));
        auto peer_b = require_bus(ivy::Bus::create("cpp-peer-b"));
        std::atomic<int> messages_a{0}, messages_b{0}, direct_a{0}, direct_b{0}, once_count{0};
        std::atomic<int> anywhere_count{0}, interval_count{0};
        auto anywhere = require_bind(bus_a.bind_unanchored(
            [&anywhere_count, &unexpected](IvyClientPtr, auto args) {
                if (args.size() != 1 || args[0] != "42")
                    ++unexpected;
                ++anywhere_count;
            }, R"(NEEDLE ([0-9]{2})$)"));
        auto interval = require_bind(bus_a.bind(
            [&interval_count, &unexpected](IvyClientPtr, auto args) {
                if (args.size() != 1 || args[0] != "2")
                    ++unexpected;
                ++interval_count;
            }, R"(^RANGE ((?I1#3i))$)"));
        auto message_a = require_bind(bus_a.bind(
            [token = std::make_unique<int>(42), &messages_a, &unexpected]
            (IvyClientPtr, std::span<const std::string_view> args) {
                if (*token != 42 || args.size() != 1 || args[0] != "42")
                    ++unexpected;
                ++messages_a;
            }, R"(^CPP ([0-9]{2}) 100%$)"));
        auto message_b = require_bind(bus_b.bind(
            [token = std::make_unique<int>(17), &messages_b, &unexpected](IvyClientPtr, auto args) {
                if (*token != 17 || args.size() != 1 || args[0] != "17")
                    ++unexpected;
                ++messages_b;
            }, R"(^CPP {} ([0-9]{{2}}) 100%$)", 23));
        auto direct_subscription_a = require_bind(bus_a.bind(
            [&direct_a, &unexpected](IvyClientPtr, int id, std::string_view message) {
                if (id != 101 || message != "direct-a")
                    ++unexpected;
                ++direct_a;
            }));
        auto direct_subscription_b = require_bind(bus_b.bind(
            [&direct_b, &unexpected](IvyClientPtr, int id, std::string_view message) {
                if (id != 102 || message != "direct-b")
                    ++unexpected;
                ++direct_b;
            }));
        std::optional<ivy::Subscription> once;
        once.emplace(require_bind(bus_a.bind([&once, &once_count](IvyClientPtr, auto) {
            once.reset();
            ++once_count;
        }, "^ONCE$")));

        // Concurrent registration must preserve each caller's formatted regexp.
        constexpr int worker_count = 4;
        constexpr int registrations = 16;
        std::array<std::vector<ivy::Subscription>, worker_count> parallel;
        std::barrier gate(worker_count);
        std::vector<std::thread> registrars;
        for (int worker = 0; worker < worker_count; ++worker) {
            registrars.emplace_back([&, worker] {
                gate.arrive_and_wait();
                for (int item = 0; item < registrations; ++item)
                    parallel[worker].push_back(require_bind(bus_a.bind(
                        [](IvyClientPtr, auto) {}, R"(^PAR {} {} ([0-9]{{2}})$)", worker, item)));
            });
        }
        for (auto& registrar : registrars)
            registrar.join();
        registrars.clear();
        for (int worker = 0; worker < worker_count; ++worker) {
            registrars.emplace_back([&, worker] {
                gate.arrive_and_wait();
                for (int item = 0; item < registrations; ++item)
                    require_start(parallel[worker][item].change(
                        R"(^PAR_CHANGED {} {} ([0-9]{{2}})$)", worker, item), "parallel change");
            });
        }
        for (auto& registrar : registrars)
            registrar.join();
        context_a = bus_a.native_handle();
        context_b = bus_b.native_handle();
        assert(context_a != context_b);

        std::string address_with_suffix = std::string(argv[1]) + "-unused";
        require_start(bus_a.start(std::string_view(address_with_suffix).substr(
                          0, std::string_view(argv[1]).size())), "start bus A");
        require_start(bus_b.start(argv[2]), "start bus B");
        if (setenv("IVYBUS", argv[1], 1) != 0)
            throw std::runtime_error("setenv IVYBUS failed");
        require_start(peer_a.start(), "start peer A");
        require_start(peer_b.start(argv[2]), "start peer B");

        // The moved bus must outlive the C loop that borrows its context.
        ivy::Bus moved_b(std::move(bus_b));
        assert(!bus_b.native_handle());
        assert(moved_b.native_handle() == context_b);
        Loop loop_a(bus_a), loop_b(moved_b), loop_peer_a(peer_a), loop_peer_b(peer_b);
        wait_for([&] { return connected_a == 1 && connected_b == 1; }, "connection callbacks timed out");
        assert(unexpected == 0);

        IvyClientPtr receiver_a = nullptr;
        IvyClientPtr receiver_b = nullptr;
        char name_a[] = "cpp-receiver-a";
        char name_b[] = "cpp-receiver-b";
        wait_for([&] {
            receiver_a = IvyContextGetApplication(peer_a.native_handle(), name_a);
            receiver_b = IvyContextGetApplication(peer_b.native_handle(), name_b);
            return receiver_a && receiver_b;
        }, "peer lookup timed out");

        assert(!peer_a.send_die(receiver_b));
        assert(!peer_a.send_error(receiver_b, 7, "wrong context"));
        require_start(peer_a.send_error(receiver_a, 7, "expected test error {}", 42), "send error frame");




        wait_for([&] {
            return advertises(peer_a, receiver_a, "^ONCE$") &&
                advertises(peer_b, receiver_b, R"(^CPP 23 ([0-9]{2}) 100%$)");
        }, "subscription advertisement timed out");
        for (int worker = 0; worker < worker_count; ++worker)
            for (int item = 0; item < registrations; ++item) {
                const auto pattern = std::format(R"(^PAR_CHANGED {} {} ([0-9]{{2}})$)", worker, item);
                wait_for([&] { return advertises(peer_a, receiver_a, pattern); },
                         "concurrent registration corrupted a regexp");
            }

        assert(message_a.is_bound() && message_b.is_bound());
        const auto no_match = peer_a.send_report("NO_MATCH");
        assert(!no_match.error && no_match.matched == 0 && no_match.accepted == 0);
        assert(!peer_a.send(receiver_b, 99, "wrong context"));
        assert(peer_a.send("prefix NEEDLE 42").value() == 1);
        assert(peer_a.send("RANGE 2").value() == 1);
        wait_for([&] { return anywhere_count == 1 && interval_count == 1; }, "anchoring modes timed out");
        assert(peer_a.send("CPP 42 100%").value() == 1);
        assert(peer_b.send("CPP {} {} 100%", 23, 17).value() == 1);
        wait_for([&] { return messages_a == 1 && messages_b == 1; }, "message callbacks timed out");
        require_start(message_a.change(R"(^UPDATED ([0-9]{2}) 100%$)"), "change regexp");
        wait_for([&] {
            return advertises(peer_a, receiver_a, R"(^UPDATED ([0-9]{2}) 100%$)") &&
                !advertises(peer_a, receiver_a, R"(^CPP ([0-9]{2}) 100%$)");
        }, "changed regexp advertisement timed out");
        assert(peer_a.send("CPP 42 100%").value() == 0);
        assert(peer_a.send("UPDATED 42 100%").value() == 1);
        wait_for([&] { return messages_a == 2; }, "callback after change timed out");
        require_start(message_a.change(R"(^UPDATED {} ([0-9]{{2}}) 100%$)", 77), "formatted change");
        wait_for([&] {
            return advertises(peer_a, receiver_a, R"(^UPDATED 77 ([0-9]{2}) 100%$)");
        }, "formatted change advertisement timed out");
        assert(peer_a.send("UPDATED 77 42 100%").value() == 1);
        wait_for([&] { return messages_a == 3; }, "callback after formatted change timed out");
        auto rejected = message_a.change("^UPDATED|UNANCHORED");
        assert(!rejected && rejected.error() == ivy::make_error_code(IVY_EUNANCHORED));
        assert(peer_a.send("UPDATED 77 42 100%").value() == 1);
        wait_for([&] { return messages_a == 4; }, "failed validation changed the subscription");
        require_start(anywhere.change_unanchored(R"(UPDATED_NEEDLE ([0-9]{{2}}) {}$)", "tail"),
                      "unanchored formatted change");
        wait_for([&] {
            return advertises(peer_a, receiver_a, R"(UPDATED_NEEDLE ([0-9]{2}) tail$)");
        }, "unanchored change advertisement timed out");
        assert(peer_a.send("prefix UPDATED_NEEDLE 42 tail").value() == 1);
        wait_for([&] { return anywhere_count == 2; }, "unanchored change callback timed out");
        char text_a[] = "direct-a";
        char text_b[] = "direct-b";
        assert(peer_a.send(receiver_a, 101, text_a).has_value());
        assert(peer_b.send(receiver_b, 102, text_b).has_value());
        wait_for([&] { return direct_a == 1 && direct_b == 1; }, "direct callbacks timed out");

        std::atomic<int> replacement_count{0};
        auto replacement = require_bind(bus_a.bind(
            [&replacement_count, &unexpected](IvyClientPtr, int id, std::string_view text) {
                if (id != 103 || text != "direct-a")
                    ++unexpected;
                ++replacement_count;
            }));
        assert(!direct_subscription_a.is_bound());
        assert(direct_subscription_a.unbind());
        assert(peer_a.send(receiver_a, 103, text_a).has_value());
        wait_for([&] { return replacement_count == 1; }, "replacement direct callback timed out");
        assert(direct_a == 1);

        assert(peer_a.send("ONCE").value() == 1);
        wait_for([&] { return once_count == 1; }, "self-unbind callback timed out");
        assert(!once);
        assert(message_a.unbind());
        wait_for([&] {
            return !advertises(peer_a, receiver_a, R"(^UPDATED 77 ([0-9]{2}) 100%$)");
        }, "unsubscribe advertisement timed out");
        assert(peer_a.send("UPDATED 77 42 100%").value() == 0);
        assert(peer_b.send("CPP {} {} 100%", 23, 17).value() == 1);
        wait_for([&] { return messages_b == 2; }, "other bus stopped receiving after unbind");
        assert(messages_a == 4 && unexpected == 0);

        assert(peer_a.send_die(receiver_a));
        wait_for([&] { return died_a == 1 && bus_a.state() == IVY_CTX_STOPPED; }, "die callback A timed out");
        loop_a.finish();
        assert(bus_a.take_callback_error());
        assert(moved_b.state() == IVY_CTX_RUNNING && died_b == 0);

        assert(peer_b.send_die(receiver_b));
        wait_for([&] { return died_b == 1 && moved_b.state() == IVY_CTX_STOPPED; }, "die callback B timed out");
        loop_b.finish();
        assert(!message_b.is_bound() && message_b.unbind());
        const auto callback_error = moved_b.take_callback_error();
        assert(!callback_error && callback_error.error() == ivy::make_error_code(ivy::Error::callback_failed));
        assert(moved_b.take_callback_error());
        assert(unexpected == 0);
        std::cout << "C++ multibus integration tests passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
