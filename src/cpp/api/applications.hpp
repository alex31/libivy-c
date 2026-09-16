/* C++ interface to Ivy. See ../version.h for the copyright notice. */
/**
 * @file applications.hpp
 * @ingroup ivy_cpp_api
 * @brief Owned application snapshots and peer lookup on ivy::Bus.
 *
 * This is a section of the public API assembled by ivy.hpp.
 * Applications can continue to include only <Ivy/ivy.hpp>.
 * The declarations below are public members of ivy::Bus.
 *
 * @section cpp_queries Owned application information
 * Queries return expected results: application(peer) holds a pair (name, host),
 * applications() a vector of names, and application_regexps(peer) a vector of
 * regexps. The strings are owned copies and survive later bus/peer changes.
 * find_application(name) holds an optional borrowed peer: nullopt means not found.
 * application_info(peer) copies the advertised name, numeric IP and Ivy TCP
 * listening port without DNS lookup. The port is the peer's advertised service
 * port, not the broadcast bus's UDP port or an outbound connection's ephemeral port.
 * @code{.cpp}
 * // With a connected peer and <iostream>, inside a function returning int:
 * auto info = bus.application(peer);
 * if (!info) {
 *     std::cerr << info.error().message() << '\n';
 *     return 1;
 * }
 * const auto& [name, host] = *info;
 * std::cout << name << " on " << host << '\n';
 * auto endpoint = bus.application_info(peer);
 * if (!endpoint) {
 *     std::cerr << endpoint.error().message() << '\n';
 *     return 1;
 * }
 * std::cout << endpoint->name << " at " << endpoint->address
 *           << " port " << endpoint->port << '\n';
 * @endcode
 * See `examples/cpp/inspection.cpp` for discovery and iteration over copied lists.
 *
 */

// Direct inclusion also assembles the complete API. The owning header defines
// IVY_CPP_API_HEADERS only while inserting this section in its proper scope.
#if !defined(IVY_CPP_API_HEADERS)
#include "../ivy.hpp"
#else
#pragma once

// IVY_CPP_API_BEGIN
    /**
     * @brief Find a connected application by its advertised name.
     * @param name Name consumed during this call; embedded NUL is invalid.
     * @return First matching borrowed peer, an empty optional if not found, or
     * IVY_EINVAL/IVY_ENOMEM/IVY_ESTATE/IVY_ESTOPPED on input/allocation/lifecycle failure.
     * The handle is valid only while the peer remains connected; it does not own the peer.
     */
    [[nodiscard]] std::expected<std::optional<IvyClientPtr>, std::error_code>
    find_application(std::string_view name) const noexcept;

    /**
     * @brief Copy the advertised name and host of one peer together under the C lock.
     * @param peer Connected peer belonging to this Bus.
     * @return Owned pair (name, host), or IVY_EINVAL for an invalid peer, IVY_ENOMEM
     * on allocation failure, IVY_ESTATE on a moved-from bus or IVY_ESTOPPED after stop.
     * The strings remain valid after peer disconnection or Bus destruction. Host
     * lookup retains the C API's diagnostic strings when address resolution fails.
     */
    [[nodiscard]] std::expected<std::pair<std::string, std::string>, std::error_code>
    application(IvyClientPtr peer) const noexcept;

    /**
     * @brief Copy the application name, numeric address and advertised TCP port.
     * @param peer Connected peer belonging to this Bus.
     * @return Owned ApplicationInfo, IVY_EINVAL for an invalid/foreign peer,
     * IVY_ENOMEM on allocation failure, IVY_EIO if the socket address cannot be
     * read, IVY_ESTATE for a moved-from Bus or IVY_ESTOPPED after stop.
     * @details No reverse DNS lookup is performed. Strings survive disconnection
     * and Bus destruction. The port is zero if the handshake has not advertised
     * it yet. Names are not unique identities. Keep a borrowed peer only while its
     * connection is known to remain alive; copy this snapshot before queuing to a GUI.
     */
    [[nodiscard]] std::expected<ApplicationInfo, std::error_code>
    application_info(IvyClientPtr peer) const noexcept;

    /**
     * @brief Copy the advertised names of all currently connected applications.
     * @return Owned vector of names, or a lifecycle/allocation error. Empty is success.
     * Order is unspecified and names need not be unique. This is a snapshot from one
     * call; no separator parsing or later borrowed storage access is required.
     */
    [[nodiscard]] std::expected<std::vector<std::string>, std::error_code>
    applications() const noexcept;

    /**
     * @brief Copy the regexps currently known and accepted from one peer.
     * @param peer Connected peer belonging to this Bus.
     * @return Owned vector of regexps, or the same errors as application(). Empty is success.
     * The snapshot survives later regexp changes or disconnection. Filtered-out
     * advertisements are not present, matching the contextual C query API.
     */
    [[nodiscard]] std::expected<std::vector<std::string>, std::error_code>
    application_regexps(IvyClientPtr peer) const noexcept;
// IVY_CPP_API_END

#endif
