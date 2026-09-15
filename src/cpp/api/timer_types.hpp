/* C++ interface to Ivy. See ../version.h for the copyright notice. */
/**
 * @file timer_types.hpp
 * @brief Timer settings and scoped timer subscription lifetime.
 *
 * This is a section of the public API assembled by ivy.hpp.
 * Applications can continue to include only <Ivy/ivy.hpp>.
 */

// Direct inclusion also assembles the complete API. The owning header defines
// IVY_CPP_API_HEADERS only while inserting this section in its proper scope.
#if !defined(IVY_CPP_API_HEADERS)
#include "../ivy.hpp"
#else
#pragma once

// IVY_CPP_API_BEGIN
namespace ivy {

/// @brief Periodic timer settings used by bind(callback, every(period)).
struct Every {
    std::chrono::milliseconds period; ///< Positive period, validated by bind().
    std::optional<int> count = std::nullopt; ///< Positive invocation count, or nullopt to repeat indefinitely.
};

/// @brief One-shot timer settings used by bind(callback, after(delay)).
struct After {
    std::chrono::milliseconds delay; ///< Nonnegative delay, validated by bind().
};

/**
 * @brief Select a periodic timer; no timer is created by this helper.
 * @param period Positive period in milliseconds, e.g. std::chrono::seconds(1).
 * @return Timer settings; bind() rejects zero, negative or unrepresentable periods.
 */
[[nodiscard]] constexpr Every every(std::chrono::milliseconds period) noexcept;

/** @brief Select a periodic timer with a limited number of invocations.
 * @param period Positive interval in milliseconds.
 * @param count Positive number of callback invocations; bind() rejects zero/negative values.
 * @return Timer settings; the returned token becomes inactive on the last invocation.
 */
[[nodiscard]] constexpr Every every(std::chrono::milliseconds period, int count) noexcept;

/** @brief Select a timer that invokes its callback once.
 * @param delay Nonnegative delay in milliseconds. Zero schedules the next opportunity
 * in the native loop; bind() never invokes the callback synchronously itself.
 * @return One-shot settings, validated when bind() is called.
 */
[[nodiscard]] constexpr After after(std::chrono::milliseconds delay) noexcept;

/**
 * @brief Owns a periodic, limited or one-shot timer and its callback.
 * Destruction or unbind() disables future callbacks. A callback already in
 * progress may finish and retains its captures. Creation, cancellation and
 * period changes are allowed from another thread or from the timer callback.
 * A limited timer becomes inactive when its last callback is selected; captures
 * remain alive until that invocation finishes. The token may outlive its Bus and
 * becomes inactive after bus stop/destruction.
 * Moving/destroying a token must not race with operations on that same token.
 */
class TimerSubscription {
public:
    /// @brief Create an inactive timer token.
    TimerSubscription() noexcept;
    /// @brief Cancel this timer automatically.
    ~TimerSubscription();
    /// @brief Transfer ownership, leaving the source inactive.
    TimerSubscription(TimerSubscription&&) noexcept;
    /** @brief Cancel this timer and take ownership from the source.
     * @return This token; self-assignment leaves it unchanged.
     */
    TimerSubscription& operator=(TimerSubscription&&) noexcept;
    /// @brief Timers cannot be copied.
    TimerSubscription(const TimerSubscription&) = delete;
    /// @brief Timers cannot be copy-assigned.
    TimerSubscription& operator=(const TimerSubscription&) = delete;

    /** @brief Disable future invocations and release captures when ongoing calls finish.
     * @return Empty success, also when already inactive or the Bus no longer exists.
     * Native cleanup occurs on the loop at the next expiry, or on bus destruction.
     */
    [[nodiscard]] std::expected<void, std::error_code> unbind() noexcept;
    /** @brief Inspect this timer's registration.
     * @return False after cancellation, move, final invocation selection or bus stop/destruction.
     */
    [[nodiscard]] bool is_bound() const noexcept;
    /** @brief Restart the timer with a new period, keeping its callback and captures.
     * @param period Positive milliseconds, representable as a C long; a timer
     * created with after() also accepts zero to reschedule the pending invocation.
     * @return Empty success, IVY_ESTATE for an inactive token, IVY_ESTOPPED after
     * stop, IVY_EINVAL for an invalid period, or an allocation/native timer error.
     * Success keeps the number of invocations still remaining; it does not reset
     * the original count or reactivate an expired timer. The last callback cannot
     * rearm its own exhausted token. Failure leaves the previous schedule in place.
     * Success starts a new period;
     * a callback already in progress may finish. Concurrent changes commit in
     * completion order. If unbind wins, the timer stays inactive and this returns
     * IVY_ESTATE. Retired native timers are removed on the loop at their next expiry.
     */
    [[nodiscard]] std::expected<void, std::error_code>
    set_period(std::chrono::milliseconds period) noexcept;

private:
    struct State;
    std::shared_ptr<State> state_;
    explicit TimerSubscription(std::shared_ptr<State> state) noexcept;
    friend class Bus;
};

} // namespace ivy
// IVY_CPP_API_END

#endif
