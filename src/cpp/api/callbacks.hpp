/* C++ interface to Ivy. See ../version.h for the copyright notice. */
/**
 * @file callbacks.hpp
 * @brief Pong, remote subscription and transport error callbacks on ivy::Bus.
 *
 * This is a section of the public API assembled by ivy.hpp.
 * Applications can continue to include only <Ivy/ivy.hpp>.
 * The declarations below are public members of ivy::Bus.
 *
 * @section cpp_event_callbacks Pong, remote subscriptions and timers
 * The selector after the callback determines the event kind, even for a generic
 * lambda. Pong and remote-subscription observers each have one slot per Bus;
 * replacing one leaves the other active. Each timer is independent.
 * @code{.cpp}
 * // With a running bus and <iostream> included, inside a function returning int:
 * using namespace std::chrono_literals;
 * auto pongs = bus.bind([](IvyClientPtr, int delay_us) {
 *     std::cout << "Pong delay (negative on timeout): " << delay_us << " us\n";
 * }, ivy::pong);
 * auto changes = bus.bind([](IvyClientPtr, int id, std::string_view regexp, IvyBindEvent) {
 *     std::cout << "Remote subscription " << id << ": " << regexp << '\n';
 * }, ivy::remote_bindings);
 * auto timer = bus.bind([](std::chrono::milliseconds late) {
 *     std::cout << "Tick, " << late.count() << " ms late\n";
 * }, ivy::every(1s));
 * if (!pongs || !changes || !timer) {
 *     const auto error = !pongs ? pongs.error() : !changes ? changes.error() : timer.error();
 *     std::cerr << error.message() << '\n';
 *     return 1;
 * }
 * if (auto changed = timer->set_period(500ms); !changed) {
 *     std::cerr << changed.error().message() << '\n';
 *     return 1;
 * }
 * // Keep these three results alive while servicing the event loop.
 * @endcode
 * To initiate a ping, call `bus.send_ping(peer)` with a connected peer from this
 * bus and check its expected result. Registration alone does not send a ping.
 * `EventSubscription` supports unbind()/is_bound(); `TimerSubscription` adds
 * set_period(). Only regexp subscriptions provide change(). Use after(delay) for
 * one invocation, every(period, count) for a limited timer or every(period) for
 * indefinite repetition. set_period() preserves the remaining invocation count;
 * an expired timer cannot be rearmed. after(0ms) runs at the next loop opportunity.
 * See `examples/cpp/callbacks.cpp` for a complete program.
 *
 */

// Direct inclusion also assembles the complete API. The owning header defines
// IVY_CPP_API_HEADERS only while inserting this section in its proper scope.
#if !defined(IVY_CPP_API_HEADERS)
#include "../ivy.hpp"
#else
#pragma once

// IVY_CPP_API_BEGIN
    /// @brief Transport notification: borrowed peer (possibly null), Ivy error and OS code.
    using TransportCallback = std::move_only_function<void(IvyClientPtr, std::error_code, int)>;

    /**
     * @brief Set the optional notification for connection-level transport failures.
     * @tparam Callback Callable compatible with TransportCallback.
     * @param callback Called with peer, Ivy error and errno/WSA code. Pass {} to disable.
     * @return Empty success, IVY_ESTATE on a moved-from bus, IVY_ESTOPPED after stop,
     * or an allocation/callback-construction error.
     * The callback runs on the event-loop thread; peer can be null during setup.
     * It also reports deferred failures while flushing queued frames, not delivery
     * receipts. Replacement lets a callback already in progress finish safely.
     * No notification is promised during stop or destruction.
     */
    template<class Callback = TransportCallback>
        requires std::constructible_from<TransportCallback, Callback>
    [[nodiscard]] std::expected<void, std::error_code>
    set_transport_error_callback(Callback&& callback) noexcept;

// IVY_CPP_API_END

#endif
