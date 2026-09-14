#include "ivy.hpp"

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

bool advertises(IvyContext* context, IvyClientPtr peer, std::string_view regexp) {
    std::array<char, 16384> buffer{};
    const int size = IvyContextGetApplicationMessagesBuffer(context, peer,
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
        ivy::Bus bus_a("cpp-receiver-a", "cpp-a ready",
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
            });
        ivy::Bus bus_b("cpp-receiver-b", std::nullopt,
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
            });
        ivy::Bus peer_a("cpp-peer-a");
        ivy::Bus peer_b("cpp-peer-b");
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

        assert(IvyContextSendDieMsg(peer_a.native_handle(), receiver_a) == IVY_OK);
        wait_for([&] { return died_a == 1 && bus_a.state() == IVY_CTX_STOPPED; }, "die callback A timed out");
        loop_a.finish();
        bus_a.rethrow_callback_exception();
        assert(moved_b.state() == IVY_CTX_RUNNING && died_b == 0);

        assert(IvyContextSendDieMsg(peer_b.native_handle(), receiver_b) == IVY_OK);
        wait_for([&] { return died_b == 1 && moved_b.state() == IVY_CTX_STOPPED; }, "die callback B timed out");
        loop_b.finish();
        try {
            moved_b.rethrow_callback_exception();
            assert(false && "callback exception was lost");
        } catch (const std::runtime_error& error) {
            assert(std::string(error.what()) == "die callback failure");
        }
        moved_b.rethrow_callback_exception();
        assert(unexpected == 0);
        std::cout << "C++ multibus integration tests passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
