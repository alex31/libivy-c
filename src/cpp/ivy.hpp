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

/// @brief C++23 bus ownership, subscriptions and error results.
namespace ivy { class Bus; }

// Public sections are included in namespace or class scope as appropriate.
#define IVY_CPP_API_HEADERS
#include "api/results.hpp"
#include "api/regexp.hpp"
#include "api/subscriptions.hpp"

namespace ivy {

/**
 * @brief Owns one independent Ivy context, created through create().
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
 * @see cpp_quickstart cpp_sending cpp_results
 */
class Bus {
public:
#include "api/lifecycle.hpp"
#include "api/messages.hpp"
#include "api/send.hpp"
#include "api/callbacks.hpp"

#include "ivy_bus_private.hpp"
};

} // namespace ivy

#undef IVY_CPP_API_HEADERS

// Template and constexpr definitions required by the compiler.
#include "ivy_detail.hpp"
