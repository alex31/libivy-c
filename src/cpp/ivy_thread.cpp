/* C++ interface to Ivy. See ../version.h for the copyright notice. */
#include "ivy_thread.hpp"

namespace ivy {
struct LoopThread::State {
    std::expected<void, std::error_code> result;
};

LoopThread::LoopThread(Bus& bus, std::thread thread, std::shared_ptr<State> state) noexcept
    : bus_(&bus), thread_(std::move(thread)), state_(std::move(state)) {}

LoopThread::CreateResult LoopThread::create_impl(Bus& bus, Completion completion) noexcept {
    const auto state = bus.state();
    if (state != IVY_CTX_RUNNING)
        return std::unexpected(make_error_code(
            state == IVY_CTX_STOPPING || state == IVY_CTX_STOPPED ? IVY_ESTOPPED : IVY_ESTATE));
    try {
        auto shared = std::make_shared<State>();
        std::thread worker([&bus, shared, callback = std::move(completion)]() mutable {
            shared->result = bus.run();
            if (!shared->result && shared->result.error() == make_error_code(IVY_ESTOPPED))
                shared->result = {}; // Stop won the race with run entry.
            if (callback) {
                auto finished = detail::guard<std::expected<void, std::error_code>>(
                    make_error_code(Error::callback_failed), [&]() -> std::expected<void, std::error_code> {
                        callback();
                        return {};
                    });
                if (shared->result && !finished)
                    shared->result = finished;
            }
        });
        return LoopThread(bus, std::move(worker), std::move(shared));
    } catch (const std::system_error& error) {
        return std::unexpected(error.code());
    } catch (const std::bad_alloc&) {
        return std::unexpected(make_error_code(IVY_ENOMEM));
    }
}

LoopThread::LoopThread(LoopThread&& other) noexcept
    : bus_(std::exchange(other.bus_, nullptr)), thread_(std::move(other.thread_)),
      state_(std::move(other.state_)) {}

LoopThread& LoopThread::operator=(LoopThread&& other) noexcept {
    if (this != &other) {
        finish();
        bus_ = std::exchange(other.bus_, nullptr);
        thread_ = std::move(other.thread_);
        state_ = std::move(other.state_);
    }
    return *this;
}

std::expected<void, std::error_code> LoopThread::request_stop() noexcept {
    return bus_ ? bus_->request_stop() : std::expected<void, std::error_code>{};
}

std::expected<void, std::error_code> LoopThread::join() noexcept {
    if (thread_.joinable()) {
        if (thread_.get_id() == std::this_thread::get_id())
            return std::unexpected(make_error_code(IVY_ESTATE));
        try {
            thread_.join();
        } catch (const std::system_error& error) {
            return std::unexpected(error.code());
        }
        bus_ = nullptr;
    }
    return state_ ? state_->result : std::expected<void, std::error_code>{};
}

void LoopThread::finish() noexcept {
    if (thread_.joinable()) {
        if (thread_.get_id() == std::this_thread::get_id())
            std::terminate();
        (void)request_stop();
        (void)join();
        if (thread_.joinable())
            std::terminate();
    }
}

LoopThread::~LoopThread() { finish(); }
} // namespace ivy
