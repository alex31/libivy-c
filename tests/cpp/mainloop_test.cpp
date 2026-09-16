#include "ivy.hpp"
#include "ivyloop.h"
#include <cassert>
#include <barrier>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <sys/socket.h>
#include <unistd.h>

using namespace std::chrono_literals;

static ivy::Bus create() {
    auto result = ivy::Bus::create("run-test");
    assert(result);
    return std::move(*result);
}

static void native_backend_error() {
    auto* state = IvyChannelStateCreate();
    assert(state && IvyChannelInitFor(state) == 0);
    int sockets[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    assert(IvyChannelAddFor(state, sockets[0], nullptr, nullptr,
        [](Channel, int, void*) {}, nullptr));
    close(sockets[0]); // Deterministic EBADF in select, with a valid registered fd.
    assert(IvyMainLoopRunFor(state) == IVY_EIO);
    assert(!IvyChannelLoopIsActiveFor(state));
    assert(!IvyChannelIsLoopThreadFor(state));
    IvyChannelStateDestroy(state);
    close(sockets[1]);
}

static void native_timer_stop() {
    auto* state = IvyChannelStateCreate();
    assert(state && IvyChannelInitFor(state) == 0);
    int unwanted = 0;
    assert(TimerRepeatAfterFor(IvyChannelGetTimerState(state), 1, 0,
        [](TimerId, void* data, unsigned long) { ++*static_cast<int*>(data); }, &unwanted));
    // Timers are inserted at the head: stop must prevent the next callback.
    assert(TimerRepeatAfterFor(IvyChannelGetTimerState(state), 1, 0,
        [](TimerId, void* data, unsigned long) {
            IvyChannelStopFor(static_cast<IvyChannelState*>(data));
        }, state));
    assert(IvyMainLoopRunFor(state) == IVY_OK);
    assert(unwanted == 0);
    IvyChannelStateDestroy(state);
}

int main(int argc, char** argv) {
    assert(argc == 2);
    assert(IvyContextRun(nullptr) == IVY_EINVAL);
    {
        auto bus = create();
        assert(bus.run().error() == ivy::make_error_code(IVY_ESTATE));
        assert(bus.stop());
        assert(bus.run().error() == ivy::make_error_code(IVY_ESTOPPED));
    }
    {
        auto bus = create();
        const auto owner = std::this_thread::get_id();
        int calls = 0;
        auto timer = bus.bind_event([&, capture = std::make_unique<int>(42)](auto) {
            assert(*capture == 42);
            assert(std::this_thread::get_id() == owner);
            ++calls;
            assert(bus.run().error() == ivy::make_error_code(IVY_ESTATE));
            assert(IvyContextDestroy(bus.native_handle()) == IVY_ESTATE);
            assert(bus.stop());
        }, ivy::after(0ms));
        assert(timer && bus.start(argv[1]));
        assert(bus.run());
        assert(calls == 1 && bus.take_callback_error());
        assert(bus.run().error() == ivy::make_error_code(IVY_ESTOPPED));
    }
    {
        auto bus = create();
        auto timer = bus.bind_event([](auto) { throw std::runtime_error("callback"); }, ivy::after(0ms));
        assert(timer && bus.start(argv[1]));
        assert(bus.run()); // Callback errors are not driver errors and are not consumed.
        assert(bus.take_callback_error().error() == ivy::make_error_code(ivy::Error::callback_failed));
        assert(bus.take_callback_error());
    }
    {
        auto bus = create();
        std::mutex mutex;
        std::condition_variable changed;
        bool entered = false, release = false;
        auto timer = bus.bind_event([&](auto) {
            std::unique_lock lock(mutex);
            entered = true;
            changed.notify_all();
            assert(changed.wait_for(lock, 3s, [&] { return release; }));
        }, ivy::after(0ms));
        assert(timer && bus.start(argv[1]));
        std::thread loop([&] { assert(bus.run()); });
        {
            std::unique_lock lock(mutex);
            assert(changed.wait_for(lock, 3s, [&] { return entered; }));
            assert(bus.run().error() == ivy::make_error_code(IVY_ESTATE));
            release = true;
        }
        changed.notify_all();
        assert(bus.stop()); // Must wake an otherwise idle select, without network traffic.
        loop.join();
    }
    for (int i = 0; i < 32; ++i) {
        auto bus = create();
        assert(bus.start(argv[1]));
        std::barrier begin(3);
        std::thread loop([&] {
            begin.arrive_and_wait();
            auto result = bus.run();
            assert(result || result.error() == ivy::make_error_code(IVY_ESTOPPED));
        });
        std::thread creator([&] {
            begin.arrive_and_wait();
            for (int j = 0; j < 8; ++j) {
                auto timer = bus.bind_event([](auto) {}, ivy::after(1ms));
                assert(timer || timer.error() == ivy::make_error_code(IVY_ESTOPPED));
            }
        });
        begin.arrive_and_wait();
        assert(bus.stop()); // Races with run entry; must never resurrect the loop.
        loop.join();
        creator.join();
    }
    native_backend_error();
    native_timer_stop();
    std::cout << "Blocking run, callback errors, stop races and native polling errors passed\n";
}
