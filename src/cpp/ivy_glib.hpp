/* C++ interface to Ivy. See ../version.h for the copyright notice. */
#pragma once
#include "ivy.hpp"
#include <glib.h>

/**
 * @file ivy_glib.hpp
 * @brief Create an ivy::Bus attached to a GLib main context.
 * @section cpp_glib Using an application-owned GLib or GTK loop
 * Include this optional header and link with pkg-config package ivy-cpp-glib.
 * It replaces ivy-cpp for this application; the two C backends export the same
 * symbols and must not be loaded together. The normal header has no GLib dependency.
 *
 * create_bus() returns the same Bus as Bus::create(). Its subscriptions, timers,
 * start(), send() and stop() keep their usual interface. The C backend retains
 * the GLib context and attaches its sources on initialization. The application
 * may release its own context reference after creation. Stopping this bus leaves
 * the host loop and other buses running. Remove the bus while dispatch is stopped
 * or on its host thread, outside callbacks; join a separate loop thread first.
 *
 * The overload without a context uses the calling thread's thread-default
 * context, falling back to GLib's global default (the usual GTK case). The
 * explicit-context overload temporarily pushes that context while creating the
 * bus, and restores the previous thread default on success and failure.
 * Create on the context's owner thread or before handing it to another thread.
 * A context owned by another thread returns IVY_ESTATE without waiting.
 *
 * @code{.cpp}
 * auto created = ivy::glib::create_bus("gtk-listener", "ready");
 * if (!created) return 1;
 * auto& bus = *created;
 * auto subscription = bus.bind([](IvyClientPtr, std::span<const std::string_view> args) {
 *     // Use/copy args here. The callback executes on the GLib/GTK loop thread.
 * }, "^HELLO (.*)$");
 * if (!subscription || !bus.start()) return 1;
 * // Run the application's normal GLib/GTK loop, keeping bus and tokens alive.
 * // For a private context: ivy::glib::create_bus(context, "listener", "ready").
 * @endcode
 */
namespace ivy::glib {
namespace detail {
// Compiled only into ivy-cpp-glib, making accidental linkage to ivy-cpp fail.
class ContextScope {
public:
    explicit ContextScope(GMainContext* context) noexcept;
    ~ContextScope();
    ContextScope(const ContextScope&) = delete;
    ContextScope& operator=(const ContextScope&) = delete;
    bool acquired() const noexcept { return acquired_; }
private:
    GMainContext* context_;
    bool acquired_;
};
} // namespace detail

/**
 * @brief Create a bus attached to the supplied GLib main context.
 * @param context Borrowed context. NULL selects the thread-default/global default.
 * @param application_name Application name, copied during creation.
 * @param ready Optional ready message, copied during creation.
 * @param application_callback Application event handler; accepts move-only captures.
 * @param die_callback Remote die handler; accepts move-only captures.
 * @return Bus::CreateResult, with the errors of Bus::create(), or IVY_ESTATE
 * if another thread owns the requested GLib context.
 * @see cpp_glib
 */
template<class Application = Bus::ApplicationCallback, class Die = Bus::DieCallback>
    requires (std::constructible_from<Bus::ApplicationCallback, Application> &&
              std::constructible_from<Bus::DieCallback, Die>)
[[nodiscard]] Bus::CreateResult create_bus(GMainContext* context,
    std::string_view application_name, std::optional<std::string_view> ready = std::nullopt,
    Application&& application_callback = {}, Die&& die_callback = {}) noexcept {
    detail::ContextScope scope(context);
    if (!scope.acquired())
        return std::unexpected(make_error_code(IVY_ESTATE));
    return Bus::create(application_name, ready,
        std::forward<Application>(application_callback), std::forward<Die>(die_callback));
}

/** @brief Create a bus on the thread-default GLib context (or global default).
 * @param application_name Application name, copied during creation.
 * @param ready Optional ready message, copied during creation.
 * @param application_callback Application handler; accepts move-only captures.
 * @param die_callback Remote die handler; accepts move-only captures.
 * @return Bus::CreateResult with the errors of Bus::create(), or IVY_ESTATE
 * if another thread owns the selected context.
 * @see cpp_glib Bus::create
 */
template<class Application = Bus::ApplicationCallback, class Die = Bus::DieCallback>
    requires (std::constructible_from<Bus::ApplicationCallback, Application> &&
              std::constructible_from<Bus::DieCallback, Die>)
[[nodiscard]] Bus::CreateResult create_bus(std::string_view application_name,
    std::optional<std::string_view> ready = std::nullopt,
    Application&& application_callback = {}, Die&& die_callback = {}) noexcept {
    return create_bus(nullptr, application_name, ready,
        std::forward<Application>(application_callback), std::forward<Die>(die_callback));
}
} // namespace ivy::glib
