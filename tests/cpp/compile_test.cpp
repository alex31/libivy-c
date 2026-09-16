#include "ivy.hpp"
#include <string>
#include <vector>

#ifndef IVY_COMPILE_CASE
#define IVY_COMPILE_CASE 0
#endif

void typed_function(ivy::ConvertStatus, long, double, std::string_view, bool) noexcept {}

void compile_check(ivy::Bus& bus, ivy::Subscription& subscription, std::string dynamic) {
    auto message = [](IvyClientPtr, std::span<const std::string_view>) {};
    int id = 42;
#if IVY_COMPILE_CASE == 0
    using namespace std::chrono_literals;
    (void)bus.bind_event([](IvyClientPtr, int) {}, ivy::pong);
    (void)bus.bind_event([](IvyClientPtr, int, std::string_view, IvyBindEvent) {}, ivy::remote_bindings);
    (void)bus.bind_event([](std::chrono::milliseconds) {}, ivy::every(1s));
    (void)bus.bind_event([](auto late) { (void)late.count(); }, ivy::every(1s, 3));
    (void)bus.bind_event([](auto late) { (void)late.count(); }, ivy::after(0ms));
    (void)bus.bind_event([](IvyClientPtr, auto delay) { (void)(delay < 0); }, ivy::pong);
    (void)bus.bind_raw([](IvyClientPtr, auto args) { (void)args.size(); }, "^GENERIC (.*)");
    (void)bus.bind_event([](auto late) { (void)late.count(); }, ivy::every(1s));
    (void)bus.bind_event([](auto...) {}, ivy::pong);
    (void)bus.bind_event([](auto...) {}, ivy::remote_bindings);
    (void)bus.bind_event([](auto...) {}, ivy::every(1s));
    (void)bus.send_ping(IvyClientPtr{});
    (void)bus.send_die(IvyClientPtr{});
    (void)bus.send_error(IvyClientPtr{}, id, dynamic);
    (void)bus.send_error(IvyClientPtr{}, id, "ERROR {}", id);
    (void)bus.application(IvyClientPtr{});
    (void)bus.applications();
    (void)bus.application_regexps(IvyClientPtr{});
    (void)bus.find_application(dynamic);
    (void)ivy::validate_anchored_regexp(dynamic);
    (void)ivy::validate_anchored_regexp("^TRACK {}", id);
    (void)bus.set_filters({"TRACK", "STATUS"});
    (void)bus.set_filters("TRACK", dynamic);
    (void)bus.set_filters();
    (void)bus.set_filters(std::vector<std::string>{"TRACK", "STATUS"});
    (void)bus.add_filter(dynamic);
    (void)bus.remove_filter(dynamic);
    (void)bus.clear_filters();
    (void)bus.send(dynamic);
    (void)bus.send("100% {} unchanged");
    (void)bus.send("TRACK {} {:02d}", dynamic, id);
    (void)bus.send_report("TRACK {}", id);
    (void)bus.send(IvyClientPtr{}, id, dynamic);
    (void)bus.send(IvyClientPtr{}, id, "DIRECT {}", dynamic);
    (void)bus.bind_raw(message, R"(^TRACK ([0-9]{2}) 100%$)");
    (void)bus.bind_raw(message, R"(^TRACK {} ([0-9]{{2}})$)", id);
    constexpr std::string_view constant = "^CONSTANT (.*)";
    (void)bus.bind_raw(message, constant);
    (void)bus.bind_raw(message, ivy::runtime_regexp(dynamic));
    (void)bus.bind_raw_unanchored(message, dynamic);
    (void)bus.bind_raw_unanchored(message, "TRACK {}", id);
    (void)bus.bind_direct([](IvyClientPtr, int, std::string_view) {});
    (void)subscription.change(R"(^UPDATED ([0-9]{2})$)");
    (void)subscription.change("^UPDATED {}", id);
    (void)subscription.change(ivy::runtime_regexp(dynamic));
    (void)subscription.change_unanchored(dynamic);
    (void)subscription.change_unanchored("UPDATED {}", id);
    auto converted = [](ivy::ConvertStatus, long, double, std::string_view, bool) {};
    (void)bus.bind_convert(converted, R"(^TRACK (\S+) (\S+) (\S+) (\S+)$)");
    (void)bus.bind_convert(converted, ivy::runtime_regexp(dynamic));
    (void)bus.bind_convert(converted, R"(^TRACK {} ([0-9]{{2}}) (\S+) (\S+) (\S+)$)", id);
    (void)bus.bind_convert(typed_function, "^FUNCTION");
    (void)bus.bind_convert(&typed_function, "^POINTER");
    (void)bus.bind_convert([](ivy::ConvertStatus, IvyClientPtr, long, double, std::string_view, bool) noexcept {}, "^PEER");
    (void)bus.bind_convert([value = std::make_unique<int>(42)](ivy::ConvertStatus, long) mutable { ++*value; }, "^MOVE");
    (void)bus.bind_convert([](ivy::ConvertStatus) {}, "^EMPTY$");
    (void)bus.bind_convert([](ivy::ConvertStatus, IvyClientPtr) {}, "^PEER_ONLY$");
    (void)bus.bind_convert(std::function<void(ivy::ConvertStatus, long)>{}, "^FUNCTION");
    (void)bus.bind_convert(std::move_only_function<void(ivy::ConvertStatus, long) const & noexcept>{}, "^MOVE_FUNCTION");
#elif IVY_COMPILE_CASE == 1
    (void)bus.bind_raw(message, "TRACK (.*)");
#elif IVY_COMPILE_CASE == 2
    (void)bus.bind_raw(message, "TRACK {} (.*)", id);
#elif IVY_COMPILE_CASE == 3
    (void)subscription.change("TRACK (.*)");
#elif IVY_COMPILE_CASE == 4
    (void)subscription.change("TRACK {} (.*)", id);
#elif IVY_COMPILE_CASE == 5
    (void)bus.bind_raw(message, dynamic);
#elif IVY_COMPILE_CASE == 6
    (void)subscription.change(std::string_view(dynamic));
#elif IVY_COMPILE_CASE == 7
    (void)bus.bind_raw(message, "^TRACK {:d}", "wrong type");
#elif IVY_COMPILE_CASE == 8
    (void)bus.bind_raw(message, "^TRACK\0hidden");
#elif IVY_COMPILE_CASE == 9
    (void)subscription.change("^TRACK\0hidden");
#elif IVY_COMPILE_CASE == 10
    (void)bus.send("TRACK {:d}", "wrong type");
#elif IVY_COMPILE_CASE == 11
    (void)bus.send(IvyClientPtr{}, id, "DIRECT {:d}", "wrong type");
#elif IVY_COMPILE_CASE == 12
    (void)bus.send_report("TRACK {:d}", "wrong type");
#elif IVY_COMPILE_CASE == 13
    (void)bus.bind_event([](IvyClientPtr, int, std::string_view) {}, ivy::pong);
#elif IVY_COMPILE_CASE == 14
    (void)bus.bind_event([](IvyClientPtr, int) {}, ivy::remote_bindings);
#elif IVY_COMPILE_CASE == 15
    (void)bus.bind_event([](IvyClientPtr, int) {}, ivy::every(std::chrono::seconds(1)));
#elif IVY_COMPILE_CASE == 16
    (void)bus.bind_raw(message, "TRACK {}", ivy::pong);
#elif IVY_COMPILE_CASE == 17
    (void)bus.set_filters(42);
#elif IVY_COMPILE_CASE == 18
    (void)bus.set_filters(std::vector<int>{1, 2});
#elif IVY_COMPILE_CASE == 19
    (void)bus.send_error(IvyClientPtr{}, id, "ERROR {:d}", "wrong type");
#elif IVY_COMPILE_CASE == 20
    (void)ivy::validate_anchored_regexp("^TRACK {:d}", "wrong type");
#elif IVY_COMPILE_CASE == 21
    (void)bus.bind_event([](IvyClientPtr, int) {}, ivy::after(std::chrono::milliseconds(0)));
#elif IVY_COMPILE_CASE == 22
    (void)bus.bind_convert([](ivy::ConvertStatus, int) {}, "^VALUE (.*)$");
#elif IVY_COMPILE_CASE == 23
    (void)bus.bind_convert([](ivy::ConvertStatus, unsigned long) {}, "^VALUE (.*)$");
#elif IVY_COMPILE_CASE == 24
    (void)bus.bind_convert([](ivy::ConvertStatus, float) {}, "^VALUE (.*)$");
#elif IVY_COMPILE_CASE == 25
    (void)bus.bind_convert([](ivy::ConvertStatus, std::string) {}, "^VALUE (.*)$");
#elif IVY_COMPILE_CASE == 26
    (void)bus.bind_convert([](ivy::ConvertStatus, const char*) {}, "^VALUE (.*)$");
#elif IVY_COMPILE_CASE == 27
    (void)bus.bind_convert([](ivy::ConvertStatus, const long&) {}, "^VALUE (.*)$");
#elif IVY_COMPILE_CASE == 28
    (void)bus.bind_convert([](ivy::ConvertStatus, auto) {}, "^VALUE (.*)$");
#elif IVY_COMPILE_CASE == 29
    struct Overloaded { void operator()(ivy::ConvertStatus, long) {} void operator()(ivy::ConvertStatus, double) {} };
    (void)bus.bind_convert(Overloaded{}, "^VALUE (.*)$");
#elif IVY_COMPILE_CASE == 30
    (void)bus.bind_convert([](ivy::ConvertStatus, long) {}, "VALUE (.*)$");
#elif IVY_COMPILE_CASE == 31
    (void)bus.bind_convert([](ivy::ConvertStatus, long) {}, dynamic);
#elif IVY_COMPILE_CASE == 32
    (void)bus.bind_convert([](ivy::ConvertStatus, long) {}, "^VALUE\0hidden");
#elif IVY_COMPILE_CASE == 33
    (void)bus.bind_convert([](ivy::ConvertStatus, long) {}, "^VALUE {:d} (.*)$", "wrong type");
#elif IVY_COMPILE_CASE == 34
    (void)bus.bind_convert([](ivy::ConvertStatus, IvyClientPtr, IvyClientPtr) {}, "^VALUE (.*)$");
#elif IVY_COMPILE_CASE == 35
    (void)bus.bind_convert([](ivy::ConvertStatus, long) { return 42; }, "^VALUE (.*)$");
#elif IVY_COMPILE_CASE == 36
    (void)bus.bind_convert([](ivy::ConvertStatus, long long) {}, "^VALUE (.*)$");
#elif IVY_COMPILE_CASE == 37
    (void)bus.bind_convert([](ivy::ConvertStatus, long) {}, "VALUE {} (.*)$", id);
#elif IVY_COMPILE_CASE == 38
    (void)bus.bind_convert([](long) {}, "^MISSING_STATUS (.*)$");
#elif IVY_COMPILE_CASE == 39
    (void)bus.bind_convert([](IvyClientPtr, ivy::ConvertStatus, long) {}, "^WRONG_ORDER (.*)$");
#elif IVY_COMPILE_CASE == 40
    (void)bus.bind_convert([](const ivy::ConvertStatus&, long) {}, "^STATUS_REFERENCE (.*)$");
#elif IVY_COMPILE_CASE == 41
    (void)bus.bind_raw([](auto...) {}, ivy::pong);
#elif IVY_COMPILE_CASE == 42
    (void)bus.bind_event([](auto...) {}, "^MESSAGE (.*)$");
#elif IVY_COMPILE_CASE == 43
    (void)bus.bind_direct([](auto...) {}, "^MESSAGE (.*)$");
#elif IVY_COMPILE_CASE == 44
    (void)bus.bind(message, "^OLD_NAME (.*)$");
#elif IVY_COMPILE_CASE == 45
    (void)bus.bind_unanchored(message, "OLD_NAME (.*)$");
#endif
}
