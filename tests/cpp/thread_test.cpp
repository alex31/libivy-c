#include "ivy_thread.hpp"
#ifdef IVY_THREAD_GLIB
#include "ivy_glib.hpp"
#endif
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <latch>
#include <mutex>
#include <stdexcept>
#include <string_view>
#include <thread>

using namespace std::chrono_literals;

static ivy::Bus create() {
#ifdef IVY_THREAD_GLIB
    auto* context = g_main_context_new();
    auto result = ivy::glib::create_bus(context, "thread-test");
    g_main_context_unref(context);
#else
    auto result = ivy::Bus::create("thread-test");
#endif
    assert(result);
    return std::move(*result);
}

int main(int argc, char** argv) {
    assert(argc >= 2);
    assert(IvyContextRequestStop(nullptr) == IVY_EINVAL);
    if (argc == 3 && std::string_view(argv[2]) == "--thread-failure") {
        auto bus = create();
        assert(bus.start(argv[1]));
        auto result = ivy::LoopThread::create(bus);
        assert(!result && result.error() == std::errc::resource_unavailable_try_again);
        assert(bus.state() == IVY_CTX_RUNNING); // Failed thread creation leaves the Bus usable.
        assert(bus.stop());
        std::cout << "Native thread creation failure converted to expected\n";
        return 0;
    }
    {
        auto bus = create();
        assert(ivy::LoopThread::create(bus).error() == ivy::make_error_code(IVY_ESTATE));
        assert(bus.request_stop());
        assert(ivy::LoopThread::create(bus).error() == ivy::make_error_code(IVY_ESTOPPED));
    }
    {
        auto bus = create();
        std::mutex mutex;
        std::condition_variable changed;
        bool entered = false, release = false;
        std::atomic<int> completed = 0;
        std::thread::id callback_thread;
        auto timer = bus.bind_event([&](auto) {
            std::unique_lock lock(mutex);
            callback_thread = std::this_thread::get_id();
            entered = true; changed.notify_all();
            assert(changed.wait_for(lock, 2s, [&] { return release; }));
        }, ivy::after(0ms));
        assert(timer && bus.start(argv[1]));
        auto loop = ivy::LoopThread::create(bus, [&, owned = std::make_unique<int>(42)] {
            assert(*owned == 42 && std::this_thread::get_id() == callback_thread);
            ++completed;
        });
        assert(loop);
        {
            std::unique_lock lock(mutex);
            assert(changed.wait_for(lock, 2s, [&] { return entered; }));
        }
        auto moved = std::move(*loop);
        assert(loop->request_stop() && loop->join()); // Moved-from helper is harmless.
        assert(moved.request_stop()); // Must return before the blocked callback is released.
        assert(completed == 0);
        { std::lock_guard lock(mutex); release = true; }
        changed.notify_all();
        assert(moved.join() && moved.join());
        assert(completed == 1 && bus.take_callback_error());
    }
    {
        auto bus = create();
        std::latch published(1), checked(1);
        ivy::LoopThread* owner = nullptr;
        auto timer = bus.bind_event([&](auto) {
            published.wait();
            assert(owner->join().error() == ivy::make_error_code(IVY_ESTATE));
            assert(bus.request_stop());
            checked.count_down();
        }, ivy::after(0ms));
        assert(timer && bus.start(argv[1]));
        auto loop = ivy::LoopThread::create(bus);
        assert(loop);
        owner = &*loop;
        published.count_down(); checked.wait();
        assert(loop->join());
    }
    {
        auto bus = create();
        auto timer = bus.bind_event([](auto) { throw std::runtime_error("Ivy callback"); }, ivy::after(0ms));
        assert(timer && bus.start(argv[1]));
        auto loop = ivy::LoopThread::create(bus);
        assert(loop && loop->join());
        assert(bus.take_callback_error().error() == ivy::make_error_code(ivy::Error::callback_failed));
        assert(bus.take_callback_error());
    }
    {
        auto bus = create();
        auto timer = bus.bind_event([&](auto) { assert(bus.request_stop()); }, ivy::after(0ms));
        assert(timer && bus.start(argv[1]));
        auto loop = ivy::LoopThread::create(bus, [] { throw std::runtime_error("completion"); });
        assert(loop);
        assert(loop->join().error() == ivy::make_error_code(ivy::Error::callback_failed));
        assert(loop->join().error() == ivy::make_error_code(ivy::Error::callback_failed));
        assert(bus.take_callback_error());
    }
    {
        auto bus = create();
        assert(bus.start(argv[1]));
        struct ThrowingMove {
            ThrowingMove() = default;
            ThrowingMove(ThrowingMove&&) { throw std::runtime_error("construction"); }
            void operator()() {}
        };
        auto loop = ivy::LoopThread::create(bus, ThrowingMove{});
        assert(!loop && loop.error() == ivy::make_error_code(ivy::Error::callback_failed));
        assert(bus.stop());
    }
    for (int i = 0; i < 16; ++i) {
        auto bus = create();
        assert(bus.start(argv[1]));
        {
            auto loop = ivy::LoopThread::create(bus);
            assert(loop); // Destructor also covers stop before run entry.
        }
        assert(bus.state() == IVY_CTX_STOPPED);
    }
    {
        auto first = create(), second = create();
        assert(first.start(argv[1]) && second.start(argv[1]));
        auto a = ivy::LoopThread::create(first);
        auto b = ivy::LoopThread::create(second);
        assert(a && b);
        *a = std::move(*b); // Stops and joins first before owning second's thread.
        assert(first.state() == IVY_CTX_STOPPED);
        assert(a->request_stop() && a->join());
        assert(second.state() == IVY_CTX_STOPPED);
    }
    std::cout << "LoopThread creation, moves, async stop, callbacks and join passed\n";
}
