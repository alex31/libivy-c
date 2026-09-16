#include <Ivy/ivy_glib.hpp>
#include <chrono>
#include <iostream>
#include <memory>

using namespace std::chrono_literals;

int main(int argc, char** argv) {
    auto check = [](const auto& result) {
        if (!result) std::cerr << result.error().message() << '\n';
        return result.has_value();
    };
    std::unique_ptr<GMainLoop, decltype(&g_main_loop_unref)> loop(
        g_main_loop_new(nullptr, FALSE), g_main_loop_unref);
    auto created = ivy::glib::create_bus("cpp-glib", "ready");
    if (!check(created)) return 1;
    auto& bus = *created;
    auto messages = bus.bind_raw([](IvyClientPtr, std::span<const std::string_view> args) {
        std::cout << "Received: " << args[0] << '\n';
    }, "^HELLO (.*)$");
    auto sender = bus.bind_event([&](auto) {
        check(bus.send("HELLO from GLib"));
    }, ivy::every(200ms));
    auto stop = bus.bind_event([&](auto) {
        check(bus.stop());
        std::cout << "Ivy stopped; the GLib application loop continues\n";
    }, ivy::after(1s));
    if (!check(messages) || !check(sender) || !check(stop)) return 1;
    if (!check(argc > 1 ? bus.start(argv[1]) : bus.start())) return 1;

    // This is an application timer: it remains active after Ivy stops.
    g_timeout_add(1500, [](void* data) -> gboolean {
        std::cout << "Application timer: quitting GLib\n";
        g_main_loop_quit(static_cast<GMainLoop*>(data));
        return G_SOURCE_REMOVE;
    }, loop.get());
    g_main_loop_run(loop.get());
    return check(bus.stop()) && check(bus.take_callback_error()) ? 0 : 1;
}
