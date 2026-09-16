/* C++ interface to Ivy. See ../version.h for the copyright notice. */
/**
 * @file subscriptions.hpp
 * @ingroup ivy_cpp_api
 * @brief Message and event subscription ownership, cancellation and lifetime.
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

/** @addtogroup ivy_cpp_api
 * @{
 */

/**
 * @brief Owns one regexp subscription; destruction unsubscribes.
 *
 * Keep the returned subscription (or its expected) alive to receive messages.
 * It can outlive its Bus but becomes inactive after bus stop/destruction.
 * Moving transfers ownership and leaves the source inactive. Moving or destroying
 * an object must not race with operations on that same object.
 */
class Subscription {
public:
    /// @brief Create an inactive subscription.
    Subscription() noexcept;

    /**
     * @brief Unsubscribe automatically; no error is returned.
     *
     * Use unbind() beforehand when its result is needed.
     */
    ~Subscription();

    /// @brief Transfer ownership, leaving the source inactive.
    Subscription(Subscription&&) noexcept;

    /**
     * @brief Unsubscribe the current registration and take ownership from the source.
     * @return This subscription; self-assignment leaves it unchanged.
     */
    Subscription& operator=(Subscription&&) noexcept;

    /// @brief Subscriptions cannot be copied.
    Subscription(const Subscription&) = delete;

    /// @brief Subscriptions cannot be copy-assigned.
    Subscription& operator=(const Subscription&) = delete;

    /**
     * @brief Disable the callback and unregister this subscription.
     * @return Empty success, including when already inactive or its Bus has stopped
     * or been destroyed; otherwise an Ivy error code.
     * A callback already in progress may finish after return. Unbinding from this
     * subscription's own callback is allowed. Captures stay alive until ongoing
     * invocations finish.
     */
    [[nodiscard]] std::expected<void, std::error_code> unbind() noexcept;

    /**
     * @brief Inspect whether this token is still active.
     * @return False after unbind, move, bus stop/destruction or direct replacement.
     * The result is a snapshot; concurrent cancellation can change it.
     */
    [[nodiscard]] bool is_bound() const noexcept;

    /**
     * @brief Replace the regexp, keeping the callback and its captures.
     * @param regexp Constant anchored regexp; braces and percent signs are literal.
     * @return Empty success, IVY_ESTATE for an inactive token, IVY_ESTOPPED for a
     * stopped bus, or an input/validation/allocation error.
     * Failed validation/native change leaves the previous regexp in place.
     * Peer updates are asynchronous; messages in flight may reflect the old regexp.
     * If unbind wins a race with change, the token remains inactive and change
     * reports IVY_ESTATE. Native removal waits for ongoing changes to finish.
     * @see @ref cpp_formatting
     */
    [[nodiscard]] std::expected<void, std::error_code> change(AnchoredRegexp regexp) noexcept;

    /**
     * @brief Replace the regexp with a dynamic anchored expression.
     * @param regexp Use runtime_regexp(text); text is consumed during this call.
     * @return Same result and cancellation rules as change(AnchoredRegexp).
     */
    [[nodiscard]] std::expected<void, std::error_code> change(RuntimeRegexp regexp) noexcept;

    /**
     * @brief Replace the regexp, explicitly allowing search anywhere in a message.
     * @param regexp Text consumed during the call, without local anchoring validation.
     * @return Same lifecycle/input/allocation errors as change(); no anchoring check.
     */
    [[nodiscard]] std::expected<void, std::error_code> change_unanchored(std::string_view regexp) noexcept;

    /**
     * @brief Format and replace the anchored regexp.
     * @tparam Args Types of the formatting arguments.
     * @param format Constant format string beginning with ^.
     * @param args Values inserted into the regexp; they are not escaped.
     * @return Same result as the text overload, plus formatting errors.
     * @see @ref cpp_formatting
     */
    template<class... Args>
        requires (sizeof...(Args) > 0)
    [[nodiscard]] std::expected<void, std::error_code>
    change(AnchoredFormat<Args...> format, Args&&... args) noexcept;

    /**
     * @brief Format and replace the unanchored regexp.
     * @tparam Args Types of the formatting arguments.
     * @param format Constant format string.
     * @param args Values inserted into the regexp; they are not escaped.
     * @return Same result as the text overload, plus formatting errors.
     * @see @ref cpp_formatting
     */
    template<class... Args>
        requires (sizeof...(Args) > 0)
    [[nodiscard]] std::expected<void, std::error_code>
    change_unanchored(std::format_string<Args...> format, Args&&... args) noexcept;

private:
    struct State;
    std::shared_ptr<State> state_;
    explicit Subscription(std::shared_ptr<State> state) noexcept;
    std::expected<void, std::error_code> change_impl(std::string_view regexp, bool anchored) noexcept;
    friend class Bus;
};

/**
 * @brief Owns the bus's direct-message callback; destruction unregisters it.
 *
 * Direct messages use a peer and an integer identifier, without a regexp.
 * There is no change() operation. Replacement makes the old token inactive;
 * unbinding that old token cannot cancel the replacement.
 * Lifetime, move and cancellation rules are the same as for Subscription.
 */
class DirectSubscription {
public:
    /// @brief Create an inactive subscription.
    DirectSubscription() noexcept;

    /**
     * @brief Unsubscribe automatically; no error is returned.
     *
     * Use unbind() beforehand when its result is needed.
     */
    ~DirectSubscription();

    /// @brief Transfer ownership, leaving the source inactive.
    DirectSubscription(DirectSubscription&&) noexcept;

    /**
     * @brief Unsubscribe the current registration and take ownership from the source.
     * @return This subscription; self-assignment leaves it unchanged.
     */
    DirectSubscription& operator=(DirectSubscription&&) noexcept;

    /// @brief Subscriptions cannot be copied.
    DirectSubscription(const DirectSubscription&) = delete;

    /// @brief Subscriptions cannot be copy-assigned.
    DirectSubscription& operator=(const DirectSubscription&) = delete;

    /**
     * @brief Disable the callback and unregister this subscription.
     * @return Empty success, including when already inactive or its Bus has stopped
     * or been destroyed; otherwise an Ivy error code.
     * A callback already in progress may finish after return. Unbinding from this
     * subscription's own callback is allowed. Captures stay alive until ongoing
     * invocations finish.
     */
    [[nodiscard]] std::expected<void, std::error_code> unbind() noexcept;

    /**
     * @brief Inspect whether this token is still active.
     * @return False after unbind, move, bus stop/destruction or direct replacement.
     * The result is a snapshot; concurrent cancellation can change it.
     */
    [[nodiscard]] bool is_bound() const noexcept;

private:
    // Reuse cancellation/lifetime machinery without exposing regexp operations.
    Subscription subscription_;
    explicit DirectSubscription(Subscription subscription) noexcept;
    friend class Bus;
};

/// @brief Select the bus's pong callback in bind_event(callback, pong).
struct PongTag {};
/// @brief Pong callback selector.
inline constexpr PongTag pong{};

/// @brief Select notifications about other applications' regexp subscriptions.
struct RemoteBindingsTag {};
/// @brief Remote subscription callback selector.
inline constexpr RemoteBindingsTag remote_bindings{};

/**
 * @brief Owns one pong or remote-subscription notification callback.
 * Keep this token (or its expected) alive. Destruction or unbind() disables the
 * callback. A successful bind of the same kind replaces the old registration;
 * an old token cannot cancel its replacement. The two kinds are independent.
 * Move, lifetime and concurrent cancellation rules match DirectSubscription.
 */
class EventSubscription {
public:
    /// @brief Create an inactive token.
    EventSubscription() noexcept;
    /// @brief Disable this callback automatically.
    ~EventSubscription();
    /// @brief Transfer ownership, leaving the source inactive.
    EventSubscription(EventSubscription&&) noexcept;
    /** @brief Cancel this registration and take ownership from the source.
     * @return This token; self-assignment leaves it unchanged.
     */
    EventSubscription& operator=(EventSubscription&&) noexcept;
    /// @brief Tokens cannot be copied.
    EventSubscription(const EventSubscription&) = delete;
    /// @brief Tokens cannot be copy-assigned.
    EventSubscription& operator=(const EventSubscription&) = delete;

    /** @brief Disable the callback; an invocation already in progress may finish.
     * @return Empty success, including when inactive or its Bus has stopped or
     * been destroyed; otherwise an Ivy error. Safe from within this callback.
     */
    [[nodiscard]] std::expected<void, std::error_code> unbind() noexcept;
    /** @brief Inspect the registration.
     * @return False after cancellation, replacement, move or bus stop/destruction.
     */
    [[nodiscard]] bool is_bound() const noexcept;

private:
    Subscription subscription_;
    explicit EventSubscription(Subscription subscription) noexcept;
    friend class Bus;
};

/** @} */

} // namespace ivy
// IVY_CPP_API_END

#endif
