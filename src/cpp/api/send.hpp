/* C++ interface to Ivy. See ../version.h for the copyright notice. */
/**
 * @file send.hpp
 * @brief Broadcasts, send reports, direct messages and control sends on ivy::Bus.
 *
 * This is a section of the public API assembled by ivy.hpp.
 * Applications can continue to include only <Ivy/ivy.hpp>.
 * The declarations below are public members of ivy::Bus.
 *
 * @section cpp_sending Choosing a send operation
 * Operation | Destination | Result
 * --------- | ----------- | ------
 * `bus.send(text)` | All matching remote subscriptions | Accepted frame count or error
 * `bus.send_report(text)` | All matching remote subscriptions | Counts and errors, even on partial failure
 * `bus.send(peer, id, text)` | One connected peer, without regexp matching | Success or error
 *
 * Each call sends a message. send_report() performs a broadcast with a detailed
 * result; it does not retrieve a previous send's result and is not a direct send.
 * A frame accepted locally was written or queued; this is not a delivery receipt.
 *
 */

// Direct inclusion also assembles the complete API. The owning header defines
// IVY_CPP_API_HEADERS only while inserting this section in its proper scope.
#if !defined(IVY_CPP_API_HEADERS)
#include "../ivy.hpp"
#else
#pragma once

// IVY_CPP_API_BEGIN
    /// @brief Accepted frame count, or an error that does not retain partial counts.
    using SendResult = std::expected<std::size_t, std::error_code>;

    /**
     * @brief Broadcast unchanged text to all matching remote subscriptions.
     * @param message Text consumed during this call; braces and percent signs are literal.
     * @return Number of complete frames accepted locally; zero matches is success.
     * Any matching send failure returns an error, even if other frames succeeded.
     * Requires a running bus. IVY_ESTATE means not started or moved from;
     * IVY_ESTOPPED means stopping/stopped. NUL, newline, bytes 2/3 and byte lengths
     * at least INT_MAX are rejected with IVY_EINVAL. Transport can report IVY_EIO,
     * IVY_ENOMEM or IVY_EFIFOFULL. UTF-8 text is allowed.
     * Use send_report() when counts must remain available on partial failure.
     * @code{.cpp}
     * // With a running bus and <iostream> included:
     * auto sent = bus.send("HELLO world");
     * if (!sent)
     *     std::cerr << sent.error().message() << '\n';
     * else
     *     std::cout << *sent << " frames accepted locally\n";
     * @endcode
     * @see cpp_sending
     */
    [[nodiscard]] SendResult send(std::string_view message) noexcept;

    /**
     * @brief Broadcast once and return counts and errors, including partial failures.
     * @param message Broadcast text, with the same input rules as send(std::string_view).
     * @return SendReport containing matched, accepted and failed counts, the first
     * error and its OS error if available. An error before matching leaves zero counts.
     * This sends to matching subscriptions, not to a specific peer. Call it instead
     * of send() for that message; it does not inspect an earlier send. Retrying a
     * whole message after partial failure may duplicate already accepted frames.
     * @code{.cpp}
     * // With a running bus and <iostream> included:
     * auto report = bus.send_report("TRACK 42 1.25");
     * std::cout << report.accepted << "/" << report.matched << " accepted locally\n";
     * if (report.error) {
     *     std::cerr << report.error.message() << ": " << report.failed << " failed\n";
     *     if (report.system_error)
     *         std::cerr << report.system_error.message() << '\n';
     * }
     * @endcode
     * @see cpp_sending SendReport
     */
    [[nodiscard]] SendReport send_report(std::string_view message) noexcept;

    /**
     * @brief Send a direct message to one peer, without regexp matching.
     * @param peer Connected peer belonging to this Bus; valid until the call returns.
     * @param id Application-defined identifier passed to the peer's direct callback.
     * @param message Text consumed during the call, with the same text rules as broadcast send().
     * @return Empty success when the frame is written or queued locally, or an error.
     * There is no delivery acknowledgement or recipient count. Null/wrong-context
     * peers are invalid; lifecycle, text and transport errors follow broadcast send().
     * @code{.cpp}
     * // With a running bus, a connected peer and <iostream> included:
     * auto sent = bus.send(peer, 7, "reload");
     * if (!sent)
     *     std::cerr << sent.error().message() << '\n';
     * @endcode
     * @see cpp_sending
     */
    [[nodiscard]] std::expected<void, std::error_code>
    send(IvyClientPtr peer, int id, std::string_view message) noexcept;

    /**
     * @brief Format and broadcast to matching remote subscriptions.
     * @tparam Args Types of the formatting arguments.
     * @param format Constant std::format string.
     * @param args Values inserted into the message.
     * @return Same result as the text overload, including formatting errors.
     * @see cpp_formatting cpp_results
     */
    template<class... Args> requires (sizeof...(Args) > 0)
    [[nodiscard]] SendResult send(std::format_string<Args...> format, Args&&... args) noexcept;

    /**
     * @brief Format and broadcast once with a detailed send report.
     * @tparam Args Types of the formatting arguments.
     * @param format Constant std::format string.
     * @param args Values inserted into the message.
     * @return Same result as the text overload, including formatting errors in SendReport::error.
     * @see cpp_formatting cpp_results
     */
    template<class... Args> requires (sizeof...(Args) > 0)
    [[nodiscard]] SendReport send_report(std::format_string<Args...> format, Args&&... args) noexcept;

    /**
     * @brief Format and send a direct message to one peer.
     * @tparam Args Types of the formatting arguments.
     * @param peer Connected peer belonging to this Bus.
     * @param id Application-defined direct-message identifier.
     * @param format Constant std::format string.
     * @param args Values inserted into the message.
     * @return Same result as the text overload, including formatting errors.
     * @see cpp_formatting cpp_results
     */
    template<class... Args> requires (sizeof...(Args) > 0)
    [[nodiscard]] std::expected<void, std::error_code>
    send(IvyClientPtr peer, int id, std::format_string<Args...> format, Args&&... args) noexcept;

    /**
     * @brief Send a ping to one connected peer; the reply goes to the pong callback.
     * @param peer Connected peer belonging to this bus, valid throughout the call.
     * @return Empty success when accepted locally; IVY_ESTATE if not running or no
     * pong subscription is installed, IVY_ESTOPPED after stop, IVY_EINVAL for an
     * invalid peer, or a native send error. Success is not a received pong.
     * @see bind(Callback&&,PongTag)
     */
    [[nodiscard]] std::expected<void, std::error_code> send_ping(IvyClientPtr peer) noexcept;

    /**
     * @brief Ask a connected peer to terminate.
     * @param peer Connected peer belonging to this Bus, valid throughout the call.
     * @return Empty success on local acceptance, or lifecycle/peer/transport errors.
     * Requires a running bus. Success does not acknowledge the remote application's exit.
     */
    [[nodiscard]] std::expected<void, std::error_code> send_die(IvyClientPtr peer) noexcept;

    /**
     * @brief Send an Ivy protocol error to one connected peer.
     * @param peer Connected peer belonging to this Bus, valid throughout the call.
     * @param id Application/protocol identifier associated with the error.
     * @param message Text consumed during the call, unchanged, including braces and percent signs.
     * @return Empty success on local acceptance, or lifecycle/input/peer/transport errors.
     * Requires a running bus; text restrictions match send(). This sends a protocol
     * error frame, not a direct message or a local callback error notification.
     */
    [[nodiscard]] std::expected<void, std::error_code>
    send_error(IvyClientPtr peer, int id, std::string_view message) noexcept;

    /** @brief Format and send an Ivy protocol error to one connected peer.
     * @tparam Args Types of the format arguments.
     * @param peer Connected peer belonging to this Bus.
     * @param id Application/protocol identifier.
     * @param format Constant std::format string.
     * @param args Values inserted into the message.
     * @return Same result as the text overload, plus formatting errors.
     */
    template<class... Args> requires (sizeof...(Args) > 0)
    [[nodiscard]] std::expected<void, std::error_code>
    send_error(IvyClientPtr peer, int id, std::format_string<Args...> format, Args&&... args) noexcept;
// IVY_CPP_API_END

#endif
