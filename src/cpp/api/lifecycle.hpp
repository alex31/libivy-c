/* C++ interface to Ivy. See ../version.h for the copyright notice. */
/**
 * @file lifecycle.hpp
 * @ingroup ivy_cpp_api
 * @brief Creating, starting and stopping ivy::Bus, with a complete receiving example.
 *
 * This is a section of the public API assembled by ivy.hpp.
 * Applications can continue to include only <Ivy/ivy.hpp>.
 * The declarations below are public members of ivy::Bus.
 *
 * @section cpp_quickstart Receiving messages
 * Create a bus with ivy::Bus::create(), check the result, register a callback,
 * then start the bus and run its event loop. Keep both the bus and subscription
 * alive while the loop runs. This complete example uses result checks only.
 * @code{.cpp}
 * #include <Ivy/ivy.hpp>
 * #include <iostream>
 *
 * int main() {
 *     auto created = ivy::Bus::create("listener", "listener ready");
 *     if (!created) {
 *         std::cerr << created.error().message() << '\n';
 *         return 1;
 *     }
 *     auto& bus = *created;
 *     auto messages = bus.bind_raw(
 *         [](IvyClientPtr, std::span<const std::string_view> args) {
 *             std::cout << args[0] << '\n';
 *         }, R"(^HELLO (.*)$)");
 *     if (!messages) {
 *         std::cerr << messages.error().message() << '\n';
 *         return 1;
 *     }
 *     if (auto started = bus.start(); !started) {
 *         std::cerr << started.error().message() << '\n';
 *         return 1;
 *     }
 *     if (auto result = bus.run(); !result) { // Until stop or a die request.
 *         std::cerr << result.error().message() << '\n';
 *         return 1;
 *     }
 *     if (auto callbacks = bus.take_callback_error(); !callbacks) {
 *         std::cerr << callbacks.error().message() << '\n';
 *         return 1;
 *     }
 *     return 0;
 * }
 * @endcode
 *
 */

// Direct inclusion also assembles the complete API. The owning header defines
// IVY_CPP_API_HEADERS only while inserting this section in its proper scope.
#if !defined(IVY_CPP_API_HEADERS)
#include "../ivy.hpp"
#else
#pragma once

// IVY_CPP_API_BEGIN
    /// @brief Connection/disconnection notification: borrowed peer and application event.
    using ApplicationCallback =
        std::move_only_function<void(IvyClientPtr, IvyApplicationEvent)>;

    /// @brief Termination notification: borrowed requesting peer and integer identifier.
    using DieCallback = std::move_only_function<void(IvyClientPtr, int)>;

    /// @brief Owned bus on success, or the reason creation failed.
    using CreateResult = std::expected<Bus, std::error_code>;

    /**
     * @brief Create a bus without starting it.
     * @tparam Application Callable compatible with ApplicationCallback.
     * @tparam Die Callable compatible with DieCallback.
     * @param application_name Name advertised to peers; copied during this call.
     * @param ready Optional ready message, copied during the call. nullopt disables
     * it; an empty view means an empty message.
     * @param application_callback Connection notifications; an empty callable disables them.
     * @param die_callback Termination notifications; an empty callable disables them.
     * @return Owned bus on success; IVY_EINVAL for embedded NUL or invalid length,
     * IVY_ENOMEM for allocation failure, an Ivy creation/registration error, or
     * Error::callback_failed if user callback construction fails.
     * Callback storage is constructed inside the checked operation. Pass lambdas
     * or functors directly; use std::move for an already owned move-only callback.
     * @see @ref cpp_quickstart
     */
    template<class Application = ApplicationCallback, class Die = DieCallback>
        requires (std::constructible_from<ApplicationCallback, Application> &&
                  std::constructible_from<DieCallback, Die>)
    [[nodiscard]] static CreateResult create(
        std::string_view application_name,
        std::optional<std::string_view> ready = std::nullopt,
        Application&& application_callback = {}, Die&& die_callback = {}) noexcept;

    /// @brief Release the context and its callbacks after the external loop has finished.
    ~Bus();

    /// @brief Transfer the context and callbacks, leaving the source empty.
    Bus(Bus&&) noexcept;

    /**
     * @brief Release this context and take ownership from the source.
     * @pre Stop and join this bus's external event-loop thread first.
     * @return This bus; self-assignment leaves it unchanged.
     */
    Bus& operator=(Bus&&) noexcept;

    /// @brief Buses cannot be copied.
    Bus(const Bus&) = delete;

    /// @brief Buses cannot be copy-assigned.
    Bus& operator=(const Bus&) = delete;

    /**
     * @brief Start using IVYBUS or Ivy's default address.
     * @return Empty success or an Ivy error; a moved-from/already-started bus returns
     * IVY_ESTATE. A stopped bus cannot be restarted.
     * Does not run the event loop. Subscriptions may be registered before start().
     */
    [[nodiscard]] std::expected<void, std::error_code> start() noexcept;

    /**
     * @brief Start on a specific bus address.
     * @param bus Address such as "127:2010", copied during the call. Empty uses the
     * same address selection as start().
     * @return Same lifecycle/native errors as start(), plus IVY_EINVAL for embedded
     * NUL or invalid length, and IVY_ENOMEM for allocation failure.
     */
    [[nodiscard]] std::expected<void, std::error_code> start(std::string_view bus) noexcept;

    /**
     * @brief Request a stop and wake the event loop.
     * @return Empty success or an Ivy error; repeated stops and a moved-from bus succeed.
     * On POSIX, a call from another thread waits for the loop to observe the request.
     * Join the loop thread before destroying this bus; stop() does not join it.
     */
    [[nodiscard]] std::expected<void, std::error_code> stop() noexcept;

    /**
     * @brief Inspect the native context state.
     * @return Current state, or IVY_CTX_DESTROYED for a moved-from bus.
     */
    [[nodiscard]] IvyContextState state() const noexcept;

    /**
     * @brief Borrow the C context for event-loop integration and C operations.
     * @return Context pointer, or nullptr for a moved-from bus.
     * Do not destroy it or replace registrations owned by this wrapper. It remains
     * valid only while the owning Bus exists (ownership can move to another Bus).
     */
    [[nodiscard]] IvyContext* native_handle() const noexcept;

    /**
     * @brief Retrieve and clear the first saved callback failure.
     * @return Empty success if none was saved (also on a moved-from bus), otherwise
     * Error::callback_failed, IVY_ENOMEM for callback/capture allocation failure,
     * or IVY_EINVAL for invalid native callback data.
     * A callback failure requests stop; it never escapes into the C event loop.
     * Call after servicing/joining the loop to observe its final error. Reading
     * clears the stored error, so a later call succeeds unless another failure occurs.
     */
    [[nodiscard]] std::expected<void, std::error_code> take_callback_error() noexcept;
// IVY_CPP_API_END

#endif
