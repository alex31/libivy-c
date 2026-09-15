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
    const auto regexps = bus.application_regexps(peer);
    if (!regexps) throw std::system_error(regexps.error(), "query peer regexps");
    return std::ranges::find(*regexps, regexp) != regexps->end();
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
        const auto no_applications = peer_a.applications();
        const auto no_peer = peer_a.find_application("absent");
        assert(no_applications && no_applications->empty() && no_peer && !*no_peer);
        assert(!peer_a.application(nullptr) && !peer_a.application_regexps(nullptr));
        std::atomic<int> pongs_a{0}, pongs_b{0}, remote_added{0}, remote_changed{0}, remote_removed{0};
        std::atomic<int> ticks_a{0}, ticks_b{0}, replacement_pongs{0}, once_ticks{0}, limited_ticks{0};
        auto once_timer = require_bind(bus_a.bind([&](auto) { ++once_ticks; }, ivy::after(0ms)));
        assert(once_ticks == 0 && once_timer.is_bound());
        int observed_id = -1;
        IvyClientPtr observed_peer = nullptr;
        auto pong_a = require_bind(peer_a.bind([&](IvyClientPtr peer, int delay) {
            if (!peer || delay < 0) ++unexpected;
            ++pongs_a;
        }, ivy::pong));
        auto pong_b = require_bind(peer_b.bind([&](IvyClientPtr peer, int delay) {
            if (!peer || delay < 0) ++unexpected;
            ++pongs_b;
        }, ivy::pong));
        auto observer = require_bind(peer_a.bind(
            [&](IvyClientPtr peer, int id, std::string_view regexp, IvyBindEvent event) {
                if (!peer) ++unexpected;
                if (regexp == "^OBSERVED$" && event == IvyAddBind) {
                    observed_id = id;
                    observed_peer = peer;
                    ++remote_added;
                }
                if (regexp == "^OBSERVED_CHANGED$" && (event == IvyChangeBind || event == IvyAddBind))
                    ++remote_changed;
                if (peer == observed_peer && id == observed_id && event == IvyRemoveBind) {
                    if (!regexp.empty()) ++unexpected; // Deletion carries only the peer-side ID.
                    ++remote_removed;
                }
            }, ivy::remote_bindings));
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
            const auto found_a = peer_a.find_application(name_a);
            const auto found_b = peer_b.find_application(name_b);
            assert(found_a && found_b);
            receiver_a = *found_a ? **found_a : nullptr;
            receiver_b = *found_b ? **found_b : nullptr;
            return receiver_a && receiver_b;
        }, "peer lookup timed out");

        const auto endpoint_a = peer_a.application_info(receiver_a);
        assert(endpoint_a && endpoint_a->name == "cpp-receiver-a");
        assert(endpoint_a->address.starts_with("127.") && endpoint_a->port > 0);
        assert(!peer_a.application_info(nullptr));
        const auto wrong_endpoint = peer_a.application_info(receiver_b);
        assert(!wrong_endpoint && wrong_endpoint.error() == ivy::make_error_code(IVY_EINVAL));
        const auto info_a = peer_a.application(receiver_a);
        const auto info_b = peer_b.application(receiver_b);
        assert(info_a && info_b);
        const auto& [application_name, host] = *info_a;
        assert(application_name == "cpp-receiver-a" && !host.empty());
        assert(info_b->first == "cpp-receiver-b" && !info_b->second.empty());
        const auto names_a = peer_a.applications();
        const auto names_b = peer_b.applications();
        assert(names_a && *names_a == std::vector<std::string>{"cpp-receiver-a"});
        assert(names_b && *names_b == std::vector<std::string>{"cpp-receiver-b"});
        const auto missing = peer_a.find_application("absent");
        assert(missing && !*missing);
        const auto invalid_name = peer_a.find_application(std::string_view("bad\0name", 8));
        assert(!invalid_name && invalid_name.error() == ivy::make_error_code(IVY_EINVAL));
        auto foreign_info = peer_a.application(receiver_b);
        auto foreign_regexps = peer_a.application_regexps(receiver_b);
        assert(!foreign_info && foreign_info.error() == ivy::make_error_code(IVY_EINVAL));
        assert(!foreign_regexps && foreign_regexps.error() == ivy::make_error_code(IVY_EINVAL));
        assert(!bus_b.applications() && !bus_b.application(receiver_b) && !bus_b.find_application("absent"));
        assert(!peer_a.send_die(receiver_b));
        assert(!peer_a.send_error(receiver_b, 7, "wrong context"));
        require_start(peer_a.send_error(receiver_a, 7, "expected test error {}", 42), "send error frame");

        auto wrong_ping = peer_a.send_ping(receiver_b);
        assert(!wrong_ping && wrong_ping.error() == ivy::make_error_code(IVY_EINVAL));
        require_start(peer_a.send_ping(receiver_a), "ping A");
        require_start(peer_b.send_ping(receiver_b), "ping B");
        wait_for([&] { return pongs_a == 1 && pongs_b == 1; }, "pong callbacks timed out");
        auto new_pong = require_bind(peer_a.bind([&](IvyClientPtr, int delay) {
            if (delay < 0) ++unexpected;
            ++replacement_pongs;
        }, ivy::pong));
        assert(!pong_a.is_bound() && pong_a.unbind());
        require_start(peer_a.send_ping(receiver_a), "ping with replacement");
        wait_for([&] { return replacement_pongs == 1; }, "replacement pong timed out");
        assert(pongs_a == 1 && pong_b.is_bound());
        assert(new_pong.unbind());
        auto no_pong = peer_a.send_ping(receiver_a);
        assert(!no_pong && no_pong.error() == ivy::make_error_code(IVY_ESTATE));

        auto observed = require_bind(bus_a.bind([](auto...) {}, "^OBSERVED$"));
        wait_for([&] { return remote_added > 0; }, "remote bind notification timed out");
        require_start(observed.change("^OBSERVED_CHANGED$"), "observed change");
        wait_for([&] { return remote_changed > 0; }, "remote change notification timed out");
        assert(observed.unbind());
        wait_for([&] { return remote_removed > 0; }, "remote unbind notification timed out");
        assert(observer.unbind());

        wait_for([&] { return once_ticks == 1; }, "one-shot timer before start timed out");
        assert(!once_timer.is_bound());
        auto finite_timer = require_bind(moved_b.bind([&](auto) { ++limited_ticks; }, ivy::every(5ms, 3)));
        wait_for([&] { return limited_ticks == 3; }, "limited timer timed out");
        assert(!finite_timer.is_bound());

        // Register and change timers from a thread other than either event loop.
        auto timer_a = require_bind(bus_a.bind([&](std::chrono::milliseconds late) {
            if (late < 0ms) ++unexpected;
            ++ticks_a;
        }, ivy::every(10ms)));
        auto timer_b = require_bind(moved_b.bind([&](std::chrono::milliseconds late) {
            if (late < 0ms) ++unexpected;
            ++ticks_b;
        }, ivy::every(15ms)));
        wait_for([&] { return ticks_a >= 2 && ticks_b >= 2; }, "periodic timers timed out");
        require_start(timer_a.set_period(5ms), "change timer period");
        const auto ticks_before_change = ticks_a.load();
        wait_for([&] { return ticks_a >= ticks_before_change + 2; }, "changed timer timed out");
        assert(timer_a.unbind() && !timer_a.is_bound() && timer_b.is_bound());
        const auto other_before = ticks_b.load();
        wait_for([&] { return ticks_b >= other_before + 2; }, "other bus timer was cancelled");
        const auto cancelled_ticks = ticks_a.load();
        std::this_thread::sleep_for(30ms);
        assert(ticks_a == cancelled_ticks);
        assert(timer_b.unbind());
        assert(once_ticks == 1 && limited_ticks == 3);




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
        const auto stopped_names = bus_a.applications();
        assert(!stopped_names && stopped_names.error() == ivy::make_error_code(IVY_ESTOPPED));
        const auto stopped_lookup = bus_a.find_application("anything");
        assert(!stopped_lookup && stopped_lookup.error() == ivy::make_error_code(IVY_ESTOPPED));
        assert(info_a->first == "cpp-receiver-a" && names_a->front() == "cpp-receiver-a");
        assert(endpoint_a->name == "cpp-receiver-a" && endpoint_a->address.starts_with("127.") && endpoint_a->port > 0);
        assert(!bus_a.application_info(receiver_a));
        assert(unexpected == 0);
        std::cout << "C++ multibus integration tests passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
