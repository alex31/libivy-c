/* C++ interface to Ivy. See ../../version.h for the copyright notice. */
/**
 * @file mainloop.hpp
 * @brief Blocking event-loop execution on the calling thread.
 * @section cpp_mainloop Running the native loop
 * After start(), run() services I/O, timers and controls until stop(). No thread
 * is created. A callback or another application thread may request stop().
 * Keep the Bus alive until run() returns; stop and join a separately created
 * loop thread before moving or destroying its Bus. Never destroy it in a callback.
 *
 * @code{.cpp}
 * auto created = ivy::Bus::create("loop-example", "ready");
 * if (!created) return 1;
 * auto& bus = *created;
 * auto finish = bus.bind([&bus](std::chrono::milliseconds) {
 *     (void)bus.stop();
 * }, ivy::after(std::chrono::seconds(1)));
 * if (!finish || !bus.start()) return 1;
 * if (auto result = bus.run(); !result) return 1;
 * if (auto result = bus.take_callback_error(); !result) return 1;
 * @endcode
 *
 * With ivy-cpp-glib, run() drives the associated GLib main context, including
 * sources belonging to other buses and the application. A host GLib/GTK loop
 * can service those sources directly; see ivy_glib.hpp for simplified creation.
 */
#if !defined(IVY_CPP_API_HEADERS)
#include "../ivy.hpp"
#else
#pragma once
// IVY_CPP_API_BEGIN
    /**
     * @brief Run the blocking event loop on the calling thread until stopped.
     * @return Empty success after stop; IVY_ESTATE for a moved-from, unstarted,
     * already-driven or recursively driven bus; IVY_ESTOPPED for a stopped bus;
     * or a backend error (IVY_EIO / IVY_ENOMEM).
     * @details Backend failures request stop. A callback failure also requests
     * stop but remains separate: run() does not consume or return that failure.
     * Call take_callback_error() after run() returns to retrieve it.
     * No thread is created. Destruction/movement must not race with this call.
     * @see cpp_mainloop stop take_callback_error
     */
    [[nodiscard]] std::expected<void, std::error_code> run() noexcept;

    /**
     * @brief Request stop without waiting for the event loop to finish.
     * @return Empty success for an accepted/already requested stop or a
     * moved-from bus; otherwise the native error.
     * @details Internal locks may briefly delay the request, but this operation
     * does not wait for a running callback or join the loop thread. Keep the Bus
     * alive until run() returns and join any separate thread before destruction.
     * Useful for a GUI that must keep processing its own events during shutdown.
     * @see run stop
     */
    [[nodiscard]] std::expected<void, std::error_code> request_stop() noexcept;
// IVY_CPP_API_END
#endif
