/* C++ interface to Ivy. See ../version.h for the copyright notice. */
#ifndef IVY_CPP_HPP
#define IVY_CPP_HPP

#include "ivy.h"

#include <expected>
#include <functional>
#include <memory>
#include <new>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <system_error>
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
};

} // namespace ivy

#endif
