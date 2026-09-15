#include "ivy_glib.hpp"
#include <cassert>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>

using namespace std::chrono_literals;

static gboolean deadline(void*) {
    std::cerr << "GLib test timed out\n";
    std::abort();
}

static GSource* timeout_on(GMainContext* context, guint delay, GSourceFunc callback, void* data) {
    auto* source = g_timeout_source_new(delay);
    g_source_set_callback(source, callback, data, nullptr);
    g_source_attach(source, context);
    return source;
}

static void external_loop(const char* address, bool global) {
    auto* context = global ? g_main_context_ref(g_main_context_default()) : g_main_context_new();
    auto* loop = g_main_loop_new(context, FALSE);
    const auto owner = std::this_thread::get_id();
    auto a = global ? ivy::glib::create_bus("glib-a") : ivy::glib::create_bus(context, "glib-a");
    auto b = ivy::glib::create_bus(context, "glib-b");
    assert(a && b);
    assert(g_main_context_get_thread_default() == nullptr);
    int received = 0, ticks_after_stop = 0;
    bool first_stopped = false;
    auto subscription = a->bind([&](IvyClientPtr sender_peer, auto args) {
        const auto endpoint = a->application_info(sender_peer);
        assert(endpoint && endpoint->name == "glib-b" && endpoint->address.starts_with("127.") && endpoint->port > 0);
        assert(std::this_thread::get_id() == owner);
        assert(args.size() == 1 && args[0] == "hello");
        ++received;
        assert(b->run().error() == ivy::make_error_code(IVY_ESTATE));
        assert(a->stop());
        first_stopped = true;
    }, "^GLIB (.*)$");
    auto sender = b->bind([&, capture = std::make_unique<int>(123)](auto) {
        assert(*capture == 123 && std::this_thread::get_id() == owner);
        assert(b->send("GLIB hello"));
        if (first_stopped && ++ticks_after_stop == 3) {
            assert(g_main_loop_is_running(loop));
            assert(b->stop());
            g_main_loop_quit(loop); // Only the application quits its host loop.
        }
    }, ivy::every(10ms));
    assert(subscription && sender && a->start(address) && b->start(address));
    auto* guard = timeout_on(context, 3000, deadline, nullptr);
    int app_ticks = 0;
    auto* app = timeout_on(context, 1, [](void* data) -> gboolean {
        ++*static_cast<int*>(data);
        return G_SOURCE_CONTINUE;
    }, &app_ticks);
    g_main_loop_run(loop);
    assert(received == 1 && ticks_after_stop == 3 && app_ticks >= 3);
    assert(a->take_callback_error() && b->take_callback_error());
    g_source_destroy(app); g_source_unref(app);
    g_source_destroy(guard); g_source_unref(guard);
    g_main_loop_unref(loop);
    g_main_context_unref(context); // Buses retain their own context references.
}

int main(int argc, char** argv) {
    assert(argc == 2);
    auto* previous = g_main_context_new();
    auto* chosen = g_main_context_new();
    g_main_context_push_thread_default(previous);
    {
        auto a = ivy::glib::create_bus(chosen, "explicit");
        assert(a && g_main_context_get_thread_default() == previous);
        auto failed = ivy::glib::create_bus(chosen, std::string_view("bad\0name", 8));
        assert(!failed && g_main_context_get_thread_default() == previous);
        struct ThrowingMove {
            ThrowingMove() = default;
            ThrowingMove(ThrowingMove&&) { throw std::runtime_error("move"); }
            void operator()(IvyClientPtr, IvyApplicationEvent) {}
        };
        auto throwing = ivy::glib::create_bus(chosen, "throws", std::nullopt, ThrowingMove{});
        assert(!throwing && g_main_context_get_thread_default() == previous);
        int default_calls = 0;
        auto thread_default = ivy::glib::create_bus("thread-default");
        assert(thread_default);
        auto timer = thread_default->bind([&](auto) {
            assert(g_main_context_is_owner(previous));
            ++default_calls;
            assert(thread_default->stop());
        }, ivy::after(0ms));
        assert(timer && thread_default->start(argv[1]) && thread_default->run());
        assert(default_calls == 1);
    }
    g_main_context_pop_thread_default(previous);
    {
        std::mutex mutex;
        std::condition_variable condition;
        bool acquired = false, release = false;
        std::thread owner([&] {
            assert(g_main_context_acquire(chosen));
            std::unique_lock lock(mutex);
            acquired = true; condition.notify_all();
            assert(condition.wait_for(lock, 3s, [&] { return release; }));
            g_main_context_release(chosen);
        });
        {
            std::unique_lock lock(mutex);
            assert(condition.wait_for(lock, 3s, [&] { return acquired; }));
        }
        auto refused = ivy::glib::create_bus(chosen, "busy");
        assert(!refused && refused.error() == ivy::make_error_code(IVY_ESTATE));
        assert(g_main_context_get_thread_default() == nullptr);
        { std::lock_guard lock(mutex); release = true; }
        condition.notify_all(); owner.join();
    }
    {
        auto bus = ivy::glib::create_bus(chosen, "poll-error");
        assert(bus && bus->start(argv[1]));
        auto saved = g_main_context_get_poll_func(chosen);
        g_main_context_set_poll_func(chosen, [](GPollFD*, guint, gint) -> gint {
            errno = EBADF; return -1;
        });
        assert(bus->run().error() == ivy::make_error_code(IVY_EIO));
        assert(bus->state() == IVY_CTX_STOPPED);
        g_main_context_set_poll_func(chosen, saved);
    }
    g_main_context_unref(previous); g_main_context_unref(chosen);
    {
        auto* ca = g_main_context_new();
        auto* cb = g_main_context_new();
        auto a = ivy::glib::create_bus(ca, "thread-a");
        auto b = ivy::glib::create_bus(cb, "thread-b");
        assert(a && b);
        auto ta = a->bind([&](auto) {
            assert(g_main_context_is_owner(ca));
            assert(!g_main_context_is_owner(cb));
            assert(a->stop());
        }, ivy::after(0ms));
        auto tb = b->bind([&](auto) {
            assert(g_main_context_is_owner(cb));
            assert(!g_main_context_is_owner(ca));
            assert(b->stop());
        }, ivy::after(0ms));
        assert(ta && tb && a->start(argv[1]) && b->start(argv[1]));
        std::thread first([&] { assert(a->run()); });
        std::thread second([&] { assert(b->run()); });
        first.join(); second.join();
        g_main_context_unref(ca); g_main_context_unref(cb);
    }
    external_loop(argv[1], true);
    external_loop(argv[1], false);
    std::cout << "GLib factories, context ownership, run errors and external multibus passed\n";
}
