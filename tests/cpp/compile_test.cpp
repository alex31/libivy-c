#include "ivy.hpp"
#include <string>

#ifndef IVY_COMPILE_CASE
#define IVY_COMPILE_CASE 0
#endif

void compile_check(ivy::Bus& bus, ivy::Subscription& subscription, std::string dynamic) {
    auto message = [](IvyClientPtr, std::span<const std::string_view>) {};
    int id = 42;
#if IVY_COMPILE_CASE == 0
    (void)bus.bind(message, R"(^TRACK ([0-9]{2}) 100%$)");
    (void)bus.bind(message, R"(^TRACK {} ([0-9]{{2}})$)", id);
    constexpr std::string_view constant = "^CONSTANT (.*)";
    (void)bus.bind(message, constant);
    (void)bus.bind(message, std::string_view(dynamic));
    (void)bus.bind(message, dynamic);
    (void)bus.bind(message, "TRACK {}", id);
    (void)bus.bind([](IvyClientPtr, int, std::string_view) {});
    (void)subscription.change(R"(^UPDATED ([0-9]{2})$)");
    (void)subscription.change("^UPDATED {}", id);
    (void)subscription.change(std::string_view(dynamic));
    (void)subscription.change(dynamic);
    (void)subscription.change("UPDATED {}", id);
#elif IVY_COMPILE_CASE == 1
    (void)bus.bind(message, "TRACK (.*)");
#elif IVY_COMPILE_CASE == 2
    (void)bus.bind(message, "TRACK {} (.*)", id);
#elif IVY_COMPILE_CASE == 3
    (void)subscription.change("TRACK (.*)");
#elif IVY_COMPILE_CASE == 4
    (void)subscription.change("TRACK {} (.*)", id);
#elif IVY_COMPILE_CASE == 5
    (void)bus.bind(message, dynamic);
#elif IVY_COMPILE_CASE == 6
    (void)subscription.change(std::string_view(dynamic));
#elif IVY_COMPILE_CASE == 7
    (void)bus.bind(message, "^TRACK {:d}", "wrong type");
#elif IVY_COMPILE_CASE == 8
    (void)bus.bind(message, "^TRACK\0hidden");
#elif IVY_COMPILE_CASE == 9
    (void)subscription.change("^TRACK\0hidden");
#endif
}
