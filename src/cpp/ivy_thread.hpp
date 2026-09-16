/* C++ interface to Ivy. See ../version.h for the copyright notice. */
#pragma once
#include "ivy.hpp"
#include <thread>

/**
 * @file ivy_thread.hpp
 * @brief Optional owner of a thread running a borrowed ivy::Bus.
 * @ingroup ivy_cpp_api
 * @section cpp_loop_thread A native loop thread with checked results
 * LoopThread::create() runs an already started Bus on a new thread. The Bus
 * stays at its original address and must outlive the helper; callbacks may keep
 * references to it. Creation never moves the Bus. Keep the subscriptions alive
 * for as long as they should receive messages. Qt/GLib are not dependencies of
 * this helper. With GLib, separate threads require separate GLib contexts.
 *
 * @code{.cpp}
 * auto created = ivy::Bus::create("threaded", "ready");
 * if (!created) return 1;
 * auto& bus = *created;
 * if (!bus.start()) return 1;
 * auto loop = ivy::LoopThread::create(bus);
 * if (!loop) return 1;
 * if (!bus.send("HELLO from the caller")) return 1;
 * if (!loop->request_stop()) return 1;
 * if (!loop->join()) return 1;
 * if (!bus.take_callback_error()) return 1;
 * @endcode
 *
 * A GUI can pass a completion callback which posts a notification to its own
 * event loop. It runs on the Ivy thread after Bus::run() returns and before
 * that thread exits; it must not wait for the GUI to join the thread. Then the
 * GUI calls request_stop(), continues processing events and joins on notification.
 * The completion callback may run before create() returns. Its captures must
 * already be valid; it must not access the not-yet-published helper object.
 * Destruction is a synchronous stop/join fallback; never destroy or move-assign
 * over this helper on its own worker thread. Access/movement of the same helper
 * must not race with another operation on it. Direct Bus sends remain MT-safe.
 */
namespace ivy {

/** @brief Movable owner of a loop thread; borrows a stable, live Bus.
 * @ingroup ivy_cpp_api
 */
class LoopThread {
public:
    /// @brief Executed once on the worker after run(), including on driver failure.
    using Completion = std::move_only_function<void()>;
    /// @brief A loop thread on success, otherwise a creation error.
    using CreateResult = std::expected<LoopThread, std::error_code>;

    /**
     * @brief Start a new thread calling bus.run().
     * @param bus Started Bus; must outlive this helper and remain at this address.
     * @param completion Optional worker-side completion callback, accepting move-only captures.
     * @return Helper, IVY_ESTATE/IVY_ESTOPPED for an invalid Bus state, IVY_ENOMEM,
     * a thread-creation system error, or Error::callback_failed for callback construction.
     * @details Creation failure leaves ownership and lifetime of the Bus with
     * the caller. Driver errors after thread creation are retrieved with join().
     * A stop between creation and run entry is treated as a normal cancellation.
     */
    template<class Callback = Completion>
        requires std::constructible_from<Completion, Callback>
    [[nodiscard]] static CreateResult create(Bus& bus, Callback&& completion = {}) noexcept {
        return detail::guard<CreateResult>(make_error_code(Error::callback_failed), [&] {
            return create_impl(bus, Completion(std::forward<Callback>(completion)));
        });
    }

    /// @brief Request stop and join if still joinable; forbidden on the worker itself.
    ~LoopThread();
    /** @brief Transfer ownership of the thread without moving the borrowed Bus.
     * @param other Helper left empty after the transfer.
     */
    LoopThread(LoopThread&& other) noexcept;
    /** @brief Stop/join the previous thread before transferring ownership.
     * @param other Helper left empty after the transfer.
     * @return This helper; self-assignment leaves it unchanged.
     */
    LoopThread& operator=(LoopThread&& other) noexcept;
    /// @brief A loop thread has a single owner.
    LoopThread(const LoopThread&) = delete;
    /// @brief A loop thread has a single owner.
    LoopThread& operator=(const LoopThread&) = delete;

    /** @brief Request stop without waiting for the loop thread.
     * @return The result of Bus::request_stop(), or success after a move/join.
     */
    [[nodiscard]] std::expected<void, std::error_code> request_stop() noexcept;

    /** @brief Join the thread and retrieve its driver/completion result.
     * @return Success, the driver error, or the completion callback error. Returns
     * IVY_ESTATE if called by the worker itself. Repeated calls retain the result.
     * Bus callback errors remain separate in Bus::take_callback_error().
     * This call can wait; request_stop() does not imply completion.
     */
    [[nodiscard]] std::expected<void, std::error_code> join() noexcept;

private:
    struct State;
    Bus* bus_;
    std::thread thread_;
    std::shared_ptr<State> state_;
    LoopThread(Bus& bus, std::thread thread, std::shared_ptr<State> state) noexcept;
    static CreateResult create_impl(Bus& bus, Completion completion) noexcept;
    void finish() noexcept;
};
} // namespace ivy
