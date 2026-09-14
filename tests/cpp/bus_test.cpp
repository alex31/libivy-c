#include "ivy.hpp"

#include <cassert>
#include <algorithm>
#include <cstdarg>
#include <cstring>
#include <cerrno>
#include <exception>
#include <iostream>
#include <latch>
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <thread>
#include <utility>
#include <vector>

// A controllable C boundary lets us exercise failure and lifetime contracts
// without relying on allocation failures or a particular network setup.
struct IvyContext {
    std::string name;
    std::optional<std::string> ready;
    std::optional<std::string> address;
    IvyApplicationCallback application;
    void* application_data;
    IvyDieCallback die;
    void* die_data;
    IvyContextState state = IVY_CTX_CREATED;
    bool in_callback = false;
};

struct _clnt_lst_dict {};
static IvyStatus last_error = IVY_OK;
static IvyStatus create_error = IVY_OK;
static IvyStatus start_error = IVY_OK;
static int live_contexts = 0;
static int destroyed_contexts = 0;

extern "C" {
IvyContext* IvyContextCreate(const char* name, const char* ready,
                            IvyApplicationCallback application, void* application_data,
                            IvyDieCallback die, void* die_data) {
    last_error = create_error;
    if (create_error != IVY_OK)
        return nullptr;
    auto* ctx = new IvyContext{name, ready ? std::optional<std::string>(ready) : std::nullopt,
                               std::nullopt, application, application_data, die, die_data};
    ++live_contexts;
    return ctx;
}

int IvyContextStart(IvyContext* ctx, const char* bus) {
    if (ctx->state != IVY_CTX_CREATED)
        return IVY_ESTATE;
    if (start_error != IVY_OK)
        return start_error;
    ctx->address = bus ? std::optional<std::string>(bus) : std::nullopt;
    ctx->state = IVY_CTX_RUNNING;
    return IVY_OK;
}

int IvyContextStop(IvyContext* ctx) {
    ctx->state = IVY_CTX_STOPPED;
    return IVY_OK;
}

int IvyContextDestroy(IvyContext* ctx) {
    assert(!ctx->in_callback);
    if (ctx->state == IVY_CTX_RUNNING)
        IvyContextStop(ctx);
    delete ctx;
    --live_contexts;
    ++destroyed_contexts;
    return IVY_OK;
}

IvyContextState IvyContextGetState(const IvyContext* ctx) { return ctx->state; }
IvyStatus IvyGetLastError() { return last_error; }
















}

template<class Operation>
void expect_exception_status(IvyStatus status, Operation operation) {
    try {
        operation();
        assert(false && "expected std::system_error");
    } catch (const std::system_error& error) {
        assert(error.code() == ivy::make_error_code(status));
        assert(std::string(error.code().category().name()) == "ivy");
    }
}

void expect_error(IvyStatus status, const std::expected<void, std::error_code>& result) {
    assert(!result);
    assert(result.error() == ivy::make_error_code(status));
    assert(std::string(result.error().category().name()) == "ivy");
}

template<class Operation>
void expect_invalid_argument(Operation operation) {
    try {
        operation();
        assert(false && "expected std::invalid_argument");
    } catch (const std::invalid_argument&) {
    }
}

void application_event(IvyContext* ctx, IvyClientPtr peer, IvyApplicationEvent event) {
    assert(ctx->application);
    ctx->in_callback = true;
    ctx->application(peer, ctx->application_data, event);
    ctx->in_callback = false;
}

void die_event(IvyContext* ctx, IvyClientPtr peer, int id) {
    assert(ctx->die);
    ctx->in_callback = true;
    ctx->die(peer, ctx->die_data, id);
    ctx->in_callback = false;
}

void strings_and_lifecycle() {
    std::string name = "receiver-unused";
    std::string ready = "READY-unused";
    ivy::Bus bus(std::string_view(name).substr(0, 8),
                 std::string_view(ready).substr(0, 5));
    name.assign("changed");
    ready.assign("changed");
    auto* ctx = bus.native_handle();
    assert(ctx->name == "receiver");
    assert(ctx->ready == "READY");
    assert(!ctx->application && !ctx->die);
    assert(bus.state() == IVY_CTX_CREATED);

    std::string address = "127:2010-unused";
    assert(bus.start(std::string_view(address).substr(0, 8)));
    address.clear();
    assert(ctx->address == "127:2010");
    assert(bus.state() == IVY_CTX_RUNNING);
    expect_error(IVY_ESTATE, bus.start());
    expect_error(IVY_ESTATE, bus.start("127:2010"));
    bus.stop();
    bus.stop();
    assert(bus.state() == IVY_CTX_STOPPED);
    expect_error(IVY_ESTATE, bus.start());
    expect_error(IVY_ESTATE, bus.start("127:2010"));

    ivy::Bus absent("absent");
    ivy::Bus empty("empty", "");
    assert(!absent.native_handle()->ready);
    assert(empty.native_handle()->ready && empty.native_handle()->ready->empty());
    assert(absent.start());
    assert(empty.start(std::string_view{}));
    assert(!absent.native_handle()->address);
    assert(empty.native_handle()->address && empty.native_handle()->address->empty());

    expect_invalid_argument([] { ivy::Bus invalid(std::string_view("a\0b", 3)); });
    expect_invalid_argument([] { ivy::Bus invalid("app", std::string_view("a\0b", 3)); });
    ivy::Bus invalid_address("app");
    expect_error(IVY_EINVAL, invalid_address.start(std::string_view("a\0b", 3)));
    assert(invalid_address.state() == IVY_CTX_CREATED);
    assert(invalid_address.start("127:2010"));
}

void move_only_callbacks() {
    _clnt_lst_dict peer;
    int application_result = 0;
    int die_result = 0;
    ivy::Bus original("movable", std::nullopt,
        [value = std::make_unique<int>(40), &peer, &application_result]
        (IvyClientPtr app, IvyApplicationEvent event) mutable {
            assert(app == &peer);
            assert(event == IvyApplicationConnected);
            application_result = ++*value;
        },
        [value = std::make_unique<int>(7), &peer, &die_result](IvyClientPtr app, int id) {
            assert(app == &peer);
            die_result = *value + id;
        });
    auto* context = original.native_handle();
    void* callback_data = context->application_data;
    std::vector<ivy::Bus> buses;
    buses.reserve(1);
    buses.push_back(std::move(original));
    buses.emplace_back("force reallocation");
    assert(!original.native_handle());
    assert(original.state() == IVY_CTX_DESTROYED);
    original.stop();
    original.rethrow_callback_exception();
    expect_error(IVY_ESTATE, original.start());
    expect_error(IVY_ESTATE, original.start("127:2010"));
    assert(buses[0].native_handle() == context);
    assert(context->application_data == callback_data);
    application_event(context, &peer, IvyApplicationConnected);
    assert(application_result == 41);

    ivy::Bus target("replace me");
    assert(target.start());
    int destroyed_before = destroyed_contexts;
    target = std::move(buses[0]);
    assert(destroyed_contexts == destroyed_before + 1);
    assert(target.native_handle() == context);
    assert(!buses[0].native_handle());
    application_event(context, &peer, IvyApplicationConnected);
    die_event(context, &peer, 5);
    assert(application_result == 42 && die_result == 12);
}

void errors_and_callback_exceptions() {
    const int before = live_contexts;
    create_error = IVY_ENOMEM;
    expect_exception_status(IVY_ENOMEM, [] { ivy::Bus failed("failed"); });
    create_error = IVY_OK;
    assert(live_contexts == before);

    ivy::Bus retry("retry");
    for (IvyStatus error : {IVY_EIO, IVY_ENOMEM, IVY_EINVAL, IVY_ESTOPPED}) {
        start_error = error;
        expect_error(error, retry.start());
        expect_error(error, retry.start("127:2010"));
        assert(retry.state() == IVY_CTX_CREATED);
    }
    start_error = IVY_OK;
    assert(retry.start());

    _clnt_lst_dict peer;
    ivy::Bus throwing("throwing", std::nullopt,
        [](IvyClientPtr, IvyApplicationEvent) { throw std::runtime_error("application failed"); },
        [](IvyClientPtr, int id) { throw id; });
    assert(throwing.start());
    application_event(throwing.native_handle(), &peer, IvyApplicationConnected);
    die_event(throwing.native_handle(), &peer, 42);
    assert(throwing.state() == IVY_CTX_STOPPED);
    try {
        throwing.rethrow_callback_exception();
        assert(false);
    } catch (const std::runtime_error& error) {
        assert(std::string(error.what()) == "application failed");
    }
    throwing.rethrow_callback_exception();
    die_event(throwing.native_handle(), &peer, 77);
    try {
        throwing.rethrow_callback_exception();
        assert(false);
    } catch (int id) {
        assert(id == 77);
    }
    throwing.rethrow_callback_exception();
}

int main() {
    static_assert(std::is_same_v<decltype(std::declval<ivy::Bus&>().start()),
                                 std::expected<void, std::error_code>>);
    static_assert(std::is_same_v<decltype(std::declval<ivy::Bus&>().start("127:2010")),
                                 std::expected<void, std::error_code>>);
    static_assert(noexcept(std::declval<ivy::Bus&>().start()));
    static_assert(noexcept(std::declval<ivy::Bus&>().start("127:2010")));
    static_assert(!std::is_copy_constructible_v<ivy::Bus>);
    static_assert(!std::is_copy_assignable_v<ivy::Bus>);
    static_assert(std::is_nothrow_move_constructible_v<ivy::Bus>);
    static_assert(std::is_nothrow_move_assignable_v<ivy::Bus>);
    static_assert(std::is_nothrow_destructible_v<ivy::Bus>);
    strings_and_lifecycle();
    move_only_callbacks();
    errors_and_callback_exceptions();
    assert(live_contexts == 0);
    std::cout << "C++ bus boundary tests passed\n";
}
