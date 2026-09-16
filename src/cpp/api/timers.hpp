/* C++ interface to Ivy. See ../version.h for the copyright notice. */
/**
 * @file timers.hpp
 * @brief Periodic, limited and one-shot timer registration on ivy::Bus.
 *
 * This is a section of the public API assembled by ivy.hpp.
 * Applications can continue to include only <Ivy/ivy.hpp>.
 * The declarations below are public members of ivy::Bus.
 */

// Direct inclusion also assembles the complete API. The owning header defines
// IVY_CPP_API_HEADERS only while inserting this section in its proper scope.
#if !defined(IVY_CPP_API_HEADERS)
#include "../ivy.hpp"
#else
#pragma once

// IVY_CPP_API_BEGIN
    /// @brief Timer notification: lateness relative to its scheduled expiry, in milliseconds.
    using TimerCallback = std::move_only_function<void(std::chrono::milliseconds)>;

    /// @brief Scoped periodic timer, or its registration error.
    using TimerBindResult = std::expected<TimerSubscription, std::error_code>;

    /**
     * @brief Register a periodic timer on this bus's event loop.
     * @tparam Callback Callable compatible with TimerCallback.
     * @param callback Receives the lateness in milliseconds, not elapsed time or
     * the configured period. Empty callables are invalid.
     * @param schedule Use ivy::every(period), e.g. ivy::every(std::chrono::seconds(1)).
     * @return TimerSubscription on success; IVY_EINVAL for an invalid period/callback,
     * IVY_ESTATE for a moved-from bus, IVY_ESTOPPED after stop, or a timer/allocation error.
     * Each registration creates an independent timer, repeating indefinitely or
     * for schedule.count invocations. Keep it alive and run the bus's event loop
     * to receive ticks. Can be called before start().
     * @see TimerSubscription::set_period() cpp_event_callbacks
     */
    template<class Callback>
    [[nodiscard]] TimerBindResult bind_event(Callback&& callback, Every schedule) noexcept;

    /**
     * @brief Register one delayed invocation on this bus's event loop.
     * @tparam Callback Callable compatible with TimerCallback.
     * @param callback Receives lateness relative to the requested expiry, in milliseconds.
     * @param schedule Use ivy::after(delay); zero delay is allowed, negative is invalid.
     * @return TimerSubscription, or the same lifecycle/allocation/input errors as every().
     * The token becomes inactive when its only callback is selected. Retain it until
     * then; destruction cancels an invocation that has not yet been selected.
     */
    template<class Callback>
    [[nodiscard]] TimerBindResult bind_event(Callback&& callback, After schedule) noexcept;
// IVY_CPP_API_END

#endif
