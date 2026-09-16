/* C++ interface to Ivy. See ../version.h for the copyright notice. */
#pragma once

#include "ivy.h"

#include <chrono>
#include <concepts>
#include <cstdint>
#include <expected>
#include <format>
#include <functional>
#include <initializer_list>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>

/**
 * @file ivy.hpp
 * @brief C++23 API for Ivy: owned buses, scoped subscriptions and checked sends.
 * @ingroup ivy_cpp_api
 *
 * Include `<Ivy/ivy.hpp>` and link with `libivy-cpp`. The definitions needed by
 * templates are included automatically from ivy_detail.hpp.
 *
 * Read the API by responsibility; every linked header retains its complete
 * contracts and examples. Subscriptions use bind_raw(), bind_convert(),
 * bind_direct() or bind_event() according to the callback's purpose.
 *
 * Topic | Public header to read
 * ----- | ---------------------
 * Creation, start/stop, state and callback errors | @ref lifecycle.hpp
 * Blocking event-loop execution | @ref mainloop.hpp
 * Creation on a GLib/GTK host context (optional) | @ref ivy_glib.hpp
 * Optional native loop thread and checked join | @ref ivy_thread.hpp
 * Regexp and direct message subscriptions | @ref messages.hpp
 * Broadcasts, reports, direct/control sends | @ref send.hpp
 * Pong, remote subscriptions, transport errors | @ref callbacks.hpp
 * Periodic, limited and one-shot timers | @ref timers.hpp
 * Application lookup and owned snapshots | @ref applications.hpp
 * Application name, numeric IP and TCP port | @ref application_types.hpp
 * Filter configuration | @ref filters.hpp
 * Error codes and SendReport | @ref results.hpp
 * Regexp types and standalone validation | @ref regexp.hpp
 * Message/event token lifetimes | @ref subscriptions.hpp
 * Timer settings and token lifetime | @ref timer_types.hpp
 *
 * @section cpp_overview Complete annotated example
 * This program subscribes to HELLO messages, replies directly, observes pongs
 * and remote subscriptions, broadcasts three messages, then stops its own bus.
 * Run two instances on the same bus to see the exchanges (e.g. pass 127:2010).
 * Zero matching subscribers is a successful send. Callbacks run while the loop
 * is serviced; keep the bus and subscription results alive until it finishes.
 * Each Ivy operation points to its public header and full documentation.
 * @code{.cpp}
 * #include <Ivy/ivy.hpp>
 * #include <chrono>
 * #include <iostream>
 * #include <span>
 * #include <string_view>
 *
 * using namespace std::chrono_literals;
 *
 * int main(int argc, char** argv) {
 *     // std::expected results and error codes: api/results.hpp.
 *     auto check = [](const auto& result) {
 *         if (!result) std::cerr << result.error().message() << '\n';
 *         return result.has_value();
 *     };
 *
 *     // Bus::create(): api/lifecycle.hpp.
 *     auto created = ivy::Bus::create("cpp-tour", "cpp-tour ready");
 *     if (!check(created)) return 1;
 *     auto& bus = *created;
 *
 *     // set_filters(): api/filters.hpp. Filters this bus's remote regexp advertisements.
 *     if (!check(bus.set_filters("HELLO"))) return 1;
 *
 *     // bind_raw(callback, regexp): api/messages.hpp; token lifetime: api/subscriptions.hpp.
 *     auto messages = bus.bind_raw([&](IvyClientPtr peer, std::span<const std::string_view> args) {
 *         const auto info = bus.application(peer); // api/applications.hpp: owned (name, host).
 *         if (!check(info)) return;
 *         const auto& [name, host] = *info;
 *         if (!args.empty()) std::cout << name << '@' << host << ": " << args[0] << '\n';
 *         if (!check(bus.send(peer, 1, "ACK"))) return; // api/send.hpp: direct message.
 *         check(bus.send_ping(peer));                 // api/send.hpp: reply goes to pongs.
 *     }, R"(^HELLO (.*)$)"); // Anchoring and format rules: api/regexp.hpp.
 *
 *     // bind_direct(direct callback): api/messages.hpp. Text is borrowed during this call.
 *     auto direct = bus.bind_direct([](IvyClientPtr, int id, std::string_view text) {
 *         std::cout << "Direct " << id << ": " << text << '\n';
 *     });
 *
 *     // bind_event(callback, ivy::pong): api/callbacks.hpp.
 *     auto pongs = bus.bind_event([](IvyClientPtr, int delay_us) {
 *         std::cout << (delay_us < 0 ? "Ping timeout: " : "Pong: ") << delay_us << " us\n";
 *     }, ivy::pong);
 *
 *     // bind_event(callback, ivy::remote_bindings): api/callbacks.hpp.
 *     auto bindings = bus.bind_event([](IvyClientPtr, int id, std::string_view regexp, IvyBindEvent event) {
 *         std::cout << "Remote regexp " << id << ": " << regexp << " (event " << event << ")\n";
 *     }, ivy::remote_bindings);
 *
 *     // set_transport_error_callback(): api/callbacks.hpp.
 *     if (!check(bus.set_transport_error_callback(
 *             [](IvyClientPtr, std::error_code error, int os_error) {
 *                 std::cerr << error.message() << " (OS " << os_error << ")\n";
 *             }))) return 1;
 *
 *     // bind_event(timer): api/timers.hpp; every(period, count): api/timer_types.hpp.
 *     auto sender = bus.bind_event([&bus, sequence = 0](std::chrono::milliseconds) mutable {
 *         const auto report = bus.send_report("HELLO {}", ++sequence); // api/send.hpp: broadcast.
 *         // SendReport fields and partial failures: api/results.hpp.
 *         std::cout << report.accepted << '/' << report.matched << " accepted, "
 *                   << report.failed << " failed\n";
 *         if (report.error) std::cerr << report.error.message() << '\n';
 *         if (report.system_error) std::cerr << report.system_error.message() << '\n';
 *     }, ivy::every(1s, 3));
 *
 *     // bind_event(timer): api/timers.hpp; after(delay): api/timer_types.hpp.
 *     auto finish = bus.bind_event([&](std::chrono::milliseconds) {
 *         check(bus.stop()); // api/lifecycle.hpp: stop this bus after five seconds.
 *     }, ivy::after(5s));
 *
 *     if (!check(messages) || !check(direct) || !check(pongs) || !check(bindings)
 *         || !check(sender) || !check(finish)) return 1;
 *
 *     // start()/start(address): api/lifecycle.hpp. No loop thread is created.
 *     const auto started = argc > 1 ? bus.start(argv[1]) : bus.start();
 *     if (!check(started)) return 1;
 *
 *     // Keep all subscription results alive until the loop finishes.
 *     if (!check(bus.run())) return 1; // api/mainloop.hpp: blocking native loop.
 *     return check(bus.take_callback_error()) ? 0 : 1; // api/lifecycle.hpp.
 * }
 * @endcode
 *
 * For a smaller receiving-only example, open api/lifecycle.hpp
 * (section @ref cpp_quickstart in the generated documentation).
 */

/** @brief C++23 bus ownership, subscriptions and error results.
 * @ingroup ivy_cpp_api
 */
namespace ivy { class Bus; }

// Public sections are included in namespace or class scope as appropriate.
#define IVY_CPP_API_HEADERS
#include "api/results.hpp"
#include "api/regexp.hpp"
#include "api/subscriptions.hpp"
#include "api/timer_types.hpp"
#include "api/application_types.hpp"

namespace ivy {

/**
 * @brief Owns one independent Ivy context, created through create().
 * @ingroup ivy_cpp_api
 *
 * The bus is movable and cannot be copied. Creation does not start the bus.
 *
 * The wrapper does not create an event-loop thread. Drive the context with
 * run(), or attach it to a supported host loop using ivy_glib.hpp. Stop and join
 * any external loop thread before destroying the bus or replacing it by move
 * assignment. Do not destroy a bus from an Ivy callback. A rejected native
 * destruction terminates to avoid freeing callback storage still used by C.
 * Moving or destroying an object must not race with operations on that object.
 *
 * Peer handles are borrowed from this context and remain valid only while the
 * peer is connected. Captured references must outlive callbacks; synchronize
 * shared mutable state used from several threads.
 * @see @ref cpp_quickstart @ref cpp_sending @ref cpp_results
 */
class Bus {
public:
#include "api/lifecycle.hpp"
#include "api/mainloop.hpp"
#include "api/messages.hpp"
#include "api/send.hpp"
#include "api/callbacks.hpp"
#include "api/timers.hpp"
#include "api/applications.hpp"
#include "api/filters.hpp"

#include "ivy_bus_private.hpp"
};

} // namespace ivy

#undef IVY_CPP_API_HEADERS

// Template and constexpr definitions required by the compiler.
#include "ivy_detail.hpp"
