/* C++ interface to Ivy. See ../version.h for the copyright notice. */
#ifndef IVY_CPP_HPP
#define IVY_CPP_HPP

#include "ivy.h"

#include <concepts>
#include <expected>
#include <format>
#include <functional>
#include <memory>
#include <new>
#include <optional>
#include <span>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>

#if !defined(__cpp_lib_move_only_function) || __cpp_lib_move_only_function < 202110L
#error "The Ivy C++ wrapper requires C++23 and std::move_only_function support."
#endif

#if !defined(__cpp_lib_expected) || __cpp_lib_expected < 202202L
#error "The Ivy C++ wrapper requires std::expected support."
#endif

namespace ivy {

/// Convert a C API status to an error code in the "ivy" category.
[[nodiscard]] std::error_code make_error_code(IvyStatus status) noexcept;

class Bus;

/**
 * A scoped regexp subscription. Destruction unsubscribes.
 * A subscription can outlive its Bus; it then becomes inactive. Moving the
 * subscription transfers ownership, leaving the source inactive.
 */
class Subscription {
public:
    Subscription() noexcept;
    ~Subscription();
    Subscription(Subscription&&) noexcept;
    Subscription& operator=(Subscription&&) noexcept;
    Subscription(const Subscription&) = delete;
    Subscription& operator=(const Subscription&) = delete;

    /**
     * Disable the callback and unregister it. Idempotent, also after Bus stop
     * or destruction. A callback already in progress may finish after return.
     * It is safe to unsubscribe from within this subscription's callback.
     */
    [[nodiscard]] std::expected<void, std::error_code> unbind() noexcept;
    [[nodiscard]] bool is_bound() const noexcept;

    /**
     * Change the regexp while retaining the C handle, callback and captures.
     * Without format arguments, braces and '%' are passed unchanged.
     * An inactive token returns IVY_ESTATE; a stopped Bus returns IVY_ESTOPPED.
     * If unbind wins a race with change, the token stays inactive and change
     * reports IVY_ESTATE. Native removal waits for ongoing changes to finish.
     */
    [[nodiscard]] std::expected<void, std::error_code> change(std::string_view regexp) noexcept;

    /// Formatting and error conventions are the same as Bus::bind().
    template<class... Args>
        requires (sizeof...(Args) > 0)
    [[nodiscard]] std::expected<void, std::error_code>
    change(std::format_string<Args...> format, Args&&... args) {
        try {
            return change(std::format(format, std::forward<Args>(args)...));
        } catch (const std::bad_alloc&) {
            return std::unexpected(make_error_code(IVY_ENOMEM));
        } catch (const std::format_error&) {
            return std::unexpected(make_error_code(IVY_EINVAL));
        } catch (const std::length_error&) {
            return std::unexpected(make_error_code(IVY_EINVAL));
        }
    }


private:
    struct State;
    std::shared_ptr<State> state_;
    explicit Subscription(std::shared_ptr<State> state) noexcept;
    std::expected<void, std::error_code> change_impl(std::string_view regexp) noexcept;
    friend class Bus;
};

/**
 * A scoped registration of the Bus's single direct-message callback.
 * It has no regexp to change. Replacement makes the old token inactive.
 * Lifetime, move and cancellation rules are the same as for Subscription.
 */
class DirectSubscription {
public:
    DirectSubscription() noexcept;
    ~DirectSubscription();
    DirectSubscription(DirectSubscription&&) noexcept;
    DirectSubscription& operator=(DirectSubscription&&) noexcept;
    DirectSubscription(const DirectSubscription&) = delete;
    DirectSubscription& operator=(const DirectSubscription&) = delete;

    [[nodiscard]] std::expected<void, std::error_code> unbind() noexcept;
    [[nodiscard]] bool is_bound() const noexcept;

private:
    // Reuse cancellation/lifetime machinery without exposing regexp operations.
    Subscription subscription_;
    explicit DirectSubscription(Subscription subscription) noexcept;
    friend class Bus;
};

/**
 * Owns one independent Ivy context. Construction does not start the bus.
 *
 * The wrapper does not create an event-loop thread. While using native_handle()
 * to drive a loop, stop and join that loop before destroying the bus or
 * replacing it by move assignment. Do not destroy a bus from an Ivy callback.
 * Moving or destroying an object must not race with operations on that object.
 * Peer handles passed to callbacks are borrowed from the C context.
 */
class Bus {
public:
    using ApplicationCallback =
        std::move_only_function<void(IvyClientPtr, IvyApplicationEvent)>;
    using DieCallback = std::move_only_function<void(IvyClientPtr, int)>;
    using MessageCallback =
        std::move_only_function<void(IvyClientPtr, std::span<const std::string_view>)>;
    using DirectCallback = std::move_only_function<void(IvyClientPtr, int, std::string_view)>;
    using BindResult = std::expected<Subscription, std::error_code>;
    using DirectBindResult = std::expected<DirectSubscription, std::error_code>;

    /**
     * Strings are copied during construction; the views need not outlive it.
     * nullopt means no ready message, while an empty view means an empty message.
     * Empty callables disable their respective notifications.
     * Embedded NUL bytes are rejected with std::invalid_argument.
     * C API failures throw std::system_error carrying an Ivy status.
     */
    explicit Bus(std::string_view application_name,
                 std::optional<std::string_view> ready = std::nullopt,
                 ApplicationCallback application_callback = {},
                 DieCallback die_callback = {});
    ~Bus();

    Bus(Bus&&) noexcept;
    Bus& operator=(Bus&&) noexcept;
    Bus(const Bus&) = delete;
    Bus& operator=(const Bus&) = delete;

    /**
     * Start using IVYBUS or Ivy's default address. Does not run the event loop.
     * Returns an empty success value or an error code carrying the Ivy status.
     * A moved-from object returns IVY_ESTATE.
     */
    [[nodiscard]] std::expected<void, std::error_code> start() noexcept;
    /**
     * Start on the supplied address. An empty view has the same meaning as start().
     * Embedded NUL bytes or an unrepresentable string length return IVY_EINVAL;
     * failure to allocate the C-compatible string returns IVY_ENOMEM.
     */
    [[nodiscard]] std::expected<void, std::error_code> start(std::string_view bus) noexcept;
    /// Request a stop. Idempotent, including on a moved-from object.
    void stop();

    /**
     * Subscribe to messages matching a regexp. Without formatting arguments,
     * '%' and braces are unchanged.
     * Capture views are valid only during the callback. Keep the returned
     * Subscription (or its expected) alive to keep receiving messages.
     * An empty callable or embedded NUL returns IVY_EINVAL.
     */
    [[nodiscard]] BindResult bind(MessageCallback callback, std::string_view regexp) noexcept;
    /**
     * Register the context's single direct-message callback. A successful bind
     * replaces the previous direct subscription and makes its token inactive.
     * The message view is valid only during the callback.
     */
    [[nodiscard]] DirectBindResult bind(DirectCallback callback) noexcept;

    /**
     * Format the regexp only when formatting arguments are supplied.
     * Literal regexp braces must be doubled in this overload.
     * Standard formatting/allocation failures return IVY_EINVAL/IVY_ENOMEM;
     * other exceptions from user-defined formatters propagate to the caller.
     */
    template<class... Args>
        requires (sizeof...(Args) > 0)
    [[nodiscard]] BindResult bind(MessageCallback callback,
                                  std::format_string<Args...> format, Args&&... args) {
        try {
            return bind(std::move(callback), std::format(format, std::forward<Args>(args)...));
        } catch (const std::bad_alloc&) {
            return std::unexpected(make_error_code(IVY_ENOMEM));
        } catch (const std::format_error&) {
            return std::unexpected(make_error_code(IVY_EINVAL));
        } catch (const std::length_error&) {
            return std::unexpected(make_error_code(IVY_EINVAL));
        }
    }

    /// A moved-from object reports IVY_CTX_DESTROYED and cannot be started.
    [[nodiscard]] IvyContextState state() const noexcept;
    /// Borrowed C handle; never destroy it or alter the wrapper's registrations.
    [[nodiscard]] IvyContext* native_handle() const noexcept;

    /**
     * Rethrow and clear the first saved callback exception, if any.
     * The trampolines catch all exceptions and request a stop. Call this after
     * servicing the loop (and joining it when it ran on another thread).
     */
    void rethrow_callback_exception();

private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
    BindResult bind_impl(MessageCallback callback, std::string_view regexp) noexcept;
    friend class Subscription;
};

} // namespace ivy

#endif
