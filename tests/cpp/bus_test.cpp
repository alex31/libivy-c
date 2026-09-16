#include "ivy.hpp"

#include <cassert>
#include <algorithm>
#include <cstdarg>
#include <cstring>
#include <cerrno>
#include <exception>
#include <iostream>
#include <latch>
#include <limits>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <thread>
#include <utility>
#include <vector>

ivy::Bus require_bus(ivy::Bus::CreateResult result) {
    assert(result);
    return std::move(*result);
}

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
    std::vector<MsgRcvPtr> bindings{};
    MsgDirectCallback direct = nullptr;
    void* direct_data = nullptr;
    IvyTransportErrorCallback transport = nullptr;
    void* transport_data = nullptr;
    IvyPongCallback pong = nullptr;
    void* pong_data = nullptr;
    IvyBindCallback remote_bindings = nullptr;
    void* remote_bindings_data = nullptr;
    std::vector<TimerId> timers{};
    std::vector<std::string> filters{};
};

struct _clnt_lst_dict { IvyContext* owner = nullptr; };
struct _timer {
    IvyContext* owner;
    TimerCb callback;
    void* data;
    long period;
    bool removed = false;
};
struct _msg_rcv {
    MsgCallback callback;
    void* data;
    std::string regexp;
};

static IvyStatus last_error = IVY_OK;
static IvyStatus create_error = IVY_OK;
static IvyStatus start_error = IVY_OK;
static IvyStatus stop_error = IVY_OK;
static IvyStatus run_error = IVY_OK;
static IvyStatus transport_registration_error = IVY_OK;
static IvyStatus bind_error = IVY_OK;
static IvyStatus change_error = IVY_OK;
static IvyStatus validation_error = IVY_OK;
static int validations = 0;
static std::string validated_regexp;
static std::move_only_function<void()> during_change;
static int unbind_count = 0;
static int live_contexts = 0;
static int destroyed_contexts = 0;
static IvyStatus send_error = IVY_OK;
static IvySendReport next_send_report{};
static std::string sent_message;
static IvyClientPtr sent_peer;
static int sent_id;
static std::string control_kind;
static IvyStatus event_registration_error = IVY_OK;
static IvyStatus timer_error = IVY_OK;
static IvyStatus ping_error = IVY_OK;
static IvyStatus filter_error = IVY_OK;
static int pings = 0;
static std::move_only_function<void()> during_timer_create;
static std::move_only_function<void()> during_ping;

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

int IvyContextRun(IvyContext* ctx) {
    if (ctx->state != IVY_CTX_RUNNING) return IVY_ESTATE;
    return run_error;
}

int IvyContextStop(IvyContext* ctx) {
    if (stop_error != IVY_OK)
        return stop_error;
    ctx->state = IVY_CTX_STOPPED;
    return IVY_OK;
}

int IvyContextRequestStop(IvyContext* ctx) { return IvyContextStop(ctx); }

int IvyContextDestroy(IvyContext* ctx) {
    assert(!ctx->in_callback);
    if (ctx->state == IVY_CTX_RUNNING)
        IvyContextStop(ctx);
    for (auto* binding : ctx->bindings)
        delete binding;
    for (auto* timer : ctx->timers)
        delete timer;
    delete ctx;
    --live_contexts;
    ++destroyed_contexts;
    return IVY_OK;
}

IvyContextState IvyContextGetState(const IvyContext* ctx) { return ctx->state; }
IvyStatus IvyGetLastError() { return last_error; }

int IvyContextSetTransportErrorCallback(IvyContext* ctx, IvyTransportErrorCallback callback, void* data) {
    if (transport_registration_error != IVY_OK)
        return transport_registration_error;
    ctx->transport = callback;
    ctx->transport_data = data;
    return IVY_OK;
}

int IvyContextSendMsgEx(IvyContext*, IvySendReport* report, const char* format, ...) {
    assert(std::strcmp(format, "%.*s") == 0);
    va_list args;
    va_start(args, format);
    const int length = va_arg(args, int);
    const char* text = va_arg(args, const char*);
    sent_message.assign(text, static_cast<std::size_t>(length));
    va_end(args);
    *report = next_send_report;
    return send_error;
}

int IvyContextSendDirectMsg(IvyContext*, IvyClientPtr peer, int id, char* text) {
    sent_peer = peer;
    sent_id = id;
    sent_message = text;
    return send_error;
}

int IvyContextSendDieMsg(IvyContext* ctx, IvyClientPtr peer) {
    if (!peer || peer->owner != ctx) return IVY_EINVAL;
    control_kind = "die";
    sent_peer = peer;
    return send_error;
}

int IvyContextSendError(IvyContext* ctx, IvyClientPtr peer, int id, const char* format, ...) {
    if (!peer || peer->owner != ctx) return IVY_EINVAL;
    assert(std::strcmp(format, "%.*s") == 0);
    va_list args;
    va_start(args, format);
    const int length = va_arg(args, int);
    const char* text = va_arg(args, const char*);
    sent_message.assign(text, static_cast<std::size_t>(length));
    va_end(args);
    control_kind = "error";
    sent_peer = peer;
    sent_id = id;
    return send_error;
}

int IvyValidateAnchoredRegexp(const char* regexp) {
    ++validations;
    validated_regexp = regexp;
    last_error = regexp[0] == '^' ? validation_error : IVY_EUNANCHORED;
    return last_error;
}

MsgRcvPtr IvyContextBindMsg(IvyContext* ctx, MsgCallback callback, void* data,
                           const char* format, ...) {
    last_error = ctx->state == IVY_CTX_STOPPED ? IVY_ESTOPPED : bind_error;
    if (last_error != IVY_OK)
        return nullptr;
    // Regexps must never be used as C format strings by the wrapper.
    assert(std::strcmp(format, "%s") == 0);
    va_list args;
    va_start(args, format);
    const char* regexp = va_arg(args, const char*);
    auto* binding = new _msg_rcv{callback, data, regexp};
    va_end(args);
    ctx->bindings.push_back(binding);
    return binding;
}

int IvyContextUnbindMsg(IvyContext* ctx, MsgRcvPtr binding) {
    if (ctx->state == IVY_CTX_STOPPED)
        return IVY_ESTOPPED;
    assert(std::find(ctx->bindings.begin(), ctx->bindings.end(), binding) != ctx->bindings.end());
    std::erase(ctx->bindings, binding);
    delete binding;
    ++unbind_count;
    return IVY_OK;
}

MsgRcvPtr IvyContextChangeMsg(IvyContext* ctx, MsgRcvPtr binding, const char* format, ...) {
    last_error = ctx->state == IVY_CTX_STOPPED ? IVY_ESTOPPED : change_error;
    if (last_error != IVY_OK)
        return nullptr;
    assert(std::strcmp(format, "%s") == 0);
    va_list args;
    va_start(args, format);
    const std::string regexp(va_arg(args, const char*));
    va_end(args);
    if (during_change)
        during_change();
    assert(std::find(ctx->bindings.begin(), ctx->bindings.end(), binding) != ctx->bindings.end());
    binding->regexp = regexp;
    return binding;
}

int IvyContextBindDirectMsg(IvyContext* ctx, MsgDirectCallback callback, void* data) {
    if (ctx->state == IVY_CTX_STOPPED)
        return IVY_ESTOPPED;
    if (bind_error != IVY_OK)
        return bind_error;
    ctx->direct = callback;
    ctx->direct_data = data;
    return IVY_OK;
}

int IvyContextSetPongCallback(IvyContext* ctx, IvyPongCallback callback, void* data) {
    if (ctx->state == IVY_CTX_STOPPED) return IVY_ESTOPPED;
    if (event_registration_error != IVY_OK) return event_registration_error;
    ctx->pong = callback;
    ctx->pong_data = data;
    return IVY_OK;
}

int IvyContextSetBindCallback(IvyContext* ctx, IvyBindCallback callback, void* data) {
    if (ctx->state == IVY_CTX_STOPPED) return IVY_ESTOPPED;
    if (event_registration_error != IVY_OK) return event_registration_error;
    ctx->remote_bindings = callback;
    ctx->remote_bindings_data = data;
    return IVY_OK;
}

const char* IvyContextGetApplicationName(IvyContext* ctx, IvyClientPtr peer) {
    return peer && peer->owner == ctx ? "peer" : nullptr;
}

int IvyContextSendPing(IvyContext* ctx, IvyClientPtr peer) {
    assert(peer && peer->owner == ctx);
    if (during_ping) during_ping();
    if (!ctx->pong) return IVY_ESTATE;
    ++pings;
    return ping_error;
}

TimerId IvyContextTimerRepeatAfter(IvyContext* ctx, int count, long period, TimerCb cb, void* data) {
    assert(count == TIMER_LOOP && period >= 0);
    last_error = ctx->state == IVY_CTX_STOPPED ? IVY_ESTOPPED : timer_error;
    if (last_error != IVY_OK) return nullptr;
    auto* timer = new _timer{ctx, cb, data, period};
    ctx->timers.push_back(timer);
    if (during_timer_create) during_timer_create();
    return timer;
}

void TimerRemove(TimerId timer) {
    assert(timer->owner->in_callback); // Must never mutate C timers from unbind/set_period threads.
    timer->removed = true;
}


int IvyContextSetFilter(IvyContext* ctx, int count, const char** words) {
    if (ctx->state == IVY_CTX_STOPPED) return IVY_ESTOPPED;
    if (filter_error != IVY_OK) return filter_error;
    ctx->filters.clear();
    for (int i = 0; i < count; ++i) ctx->filters.emplace_back(words[i]);
    return IVY_OK;
}

int IvyContextAddFilter(IvyContext* ctx, const char* word) {
    if (ctx->state == IVY_CTX_STOPPED) return IVY_ESTOPPED;
    if (filter_error != IVY_OK) return filter_error;
    ctx->filters.emplace_back(word);
    return IVY_OK;
}

int IvyContextRemoveFilter(IvyContext* ctx, const char* word) {
    if (ctx->state == IVY_CTX_STOPPED) return IVY_ESTOPPED;
    if (filter_error != IVY_OK) return filter_error;
    std::erase(ctx->filters, std::string(word));
    return IVY_OK;
}

}

template<class T, class Status>
void expect_error(Status status, const std::expected<T, std::error_code>& result) {
    assert(!result);
    assert(result.error() == ivy::make_error_code(status));
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
    auto bus = require_bus(ivy::Bus::create(std::string_view(name).substr(0, 8),
                 std::string_view(ready).substr(0, 5)));
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
    assert(bus.stop());
    assert(bus.stop());
    assert(bus.state() == IVY_CTX_STOPPED);
    expect_error(IVY_ESTATE, bus.start());
    expect_error(IVY_ESTATE, bus.start("127:2010"));

    auto absent = require_bus(ivy::Bus::create("absent"));
    auto empty = require_bus(ivy::Bus::create("empty", ""));
    assert(!absent.native_handle()->ready);
    assert(empty.native_handle()->ready && empty.native_handle()->ready->empty());
    assert(absent.start());
    assert(empty.start(std::string_view{}));
    assert(!absent.native_handle()->address);
    assert(empty.native_handle()->address && empty.native_handle()->address->empty());

    expect_error(IVY_EINVAL, ivy::Bus::create(std::string_view("a\0b", 3)));
    expect_error(IVY_EINVAL, ivy::Bus::create("app", std::string_view("a\0b", 3)));
    auto invalid_address = require_bus(ivy::Bus::create("app"));
    expect_error(IVY_EINVAL, invalid_address.start(std::string_view("a\0b", 3)));
    assert(invalid_address.state() == IVY_CTX_CREATED);
    assert(invalid_address.start("127:2010"));
}

void move_only_callbacks() {
    _clnt_lst_dict peer;
    int application_result = 0;
    int die_result = 0;
    auto original = require_bus(ivy::Bus::create("movable", std::nullopt,
        [value = std::make_unique<int>(40), &peer, &application_result]
        (IvyClientPtr app, IvyApplicationEvent event) mutable {
            assert(app == &peer);
            assert(event == IvyApplicationConnected);
            application_result = ++*value;
        },
        [value = std::make_unique<int>(7), &peer, &die_result](IvyClientPtr app, int id) {
            assert(app == &peer);
            die_result = *value + id;
        }));
    auto* context = original.native_handle();
    void* callback_data = context->application_data;
    std::vector<ivy::Bus> buses;
    buses.reserve(1);
    buses.push_back(std::move(original));
    buses.push_back(require_bus(ivy::Bus::create("force reallocation")));
    assert(!original.native_handle());
    assert(original.state() == IVY_CTX_DESTROYED);
    assert(original.stop());
    assert(original.take_callback_error());
    expect_error(IVY_ESTATE, original.start());
    expect_error(IVY_ESTATE, original.start("127:2010"));
    assert(buses[0].native_handle() == context);
    assert(context->application_data == callback_data);
    application_event(context, &peer, IvyApplicationConnected);
    assert(application_result == 41);

    auto target = require_bus(ivy::Bus::create("replace me"));
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
    expect_error(IVY_ENOMEM, ivy::Bus::create("failed"));
    create_error = IVY_OK;
    assert(live_contexts == before);

    transport_registration_error = IVY_EIO;
    expect_error(IVY_EIO, ivy::Bus::create("registration failure"));
    transport_registration_error = IVY_OK;
    assert(live_contexts == before);

    auto retry = require_bus(ivy::Bus::create("retry"));
    for (IvyStatus error : {IVY_EIO, IVY_ENOMEM, IVY_EINVAL, IVY_ESTOPPED}) {
        start_error = error;
        expect_error(error, retry.start());
        expect_error(error, retry.start("127:2010"));
        assert(retry.state() == IVY_CTX_CREATED);
    }
    start_error = IVY_OK;
    assert(retry.start());
    stop_error = IVY_EIO;
    expect_error(IVY_EIO, retry.stop());
    assert(retry.state() == IVY_CTX_RUNNING);
    stop_error = IVY_OK;
    assert(retry.stop());

    _clnt_lst_dict peer;
    auto throwing = require_bus(ivy::Bus::create("throwing", std::nullopt,
        [](IvyClientPtr, IvyApplicationEvent) { throw std::bad_alloc(); },
        [](IvyClientPtr, int id) { throw id; }));
    assert(throwing.start());
    application_event(throwing.native_handle(), &peer, IvyApplicationConnected);
    die_event(throwing.native_handle(), &peer, 42);
    assert(throwing.state() == IVY_CTX_STOPPED);
    expect_error(IVY_ENOMEM, throwing.take_callback_error());
    assert(throwing.take_callback_error());
    die_event(throwing.native_handle(), &peer, 77);
    expect_error(ivy::Error::callback_failed, throwing.take_callback_error());
    assert(throwing.take_callback_error());
}

void subscriptions_and_formats() {
    auto bus = require_bus(ivy::Bus::create("bindings"));
    auto* ctx = bus.native_handle();
    _clnt_lst_dict peer;
    std::vector<std::string> received;
    int calls = 0;
    auto callback = [count = std::make_unique<int>(0), &calls, &received, &peer]
        (IvyClientPtr app, std::span<const std::string_view> args) {
            assert(app == &peer);
            calls = ++*count;
            received.assign(args.begin(), args.end());
        };
    std::string pattern = R"(^TRACK ([0-9]{2}) 100%$-suffix)";
    auto result = bus.bind_raw(std::move(callback), ivy::runtime_regexp(std::string_view(pattern).substr(0, pattern.size() - 7)));
    assert(result && result->is_bound());
    auto* binding = ctx->bindings.back();
    assert(binding->regexp == R"(^TRACK ([0-9]{2}) 100%$)");
    const auto pending_callback = binding->callback;
    void* pending_data = binding->data;
    char value[] = "42";
    char* arguments[] = {value};
    pending_callback(&peer, pending_data, 1, arguments);
    assert(calls == 1 && received == std::vector<std::string>{"42"});
    pending_callback(&peer, pending_data, 0, nullptr);
    assert(calls == 2 && received.empty());

    auto ignore = [](IvyClientPtr, std::span<const std::string_view>) {};
    auto formatted = bus.bind_raw(ignore, R"(^TRACK {} ([0-9]{{2}}) 100%$)", 42);
    assert(formatted);
    assert(ctx->bindings.back()->regexp == R"(^TRACK 42 ([0-9]{2}) 100%$)");
    auto braces = bus.bind_raw_unanchored(ignore, "{}");
    assert(braces && ctx->bindings.back()->regexp == "{}");
    const auto count = ctx->bindings.size();
    auto invalid_format = bus.bind_raw(ignore, "^{:{}d}", 42, -1);
    assert(!invalid_format && invalid_format.error() == ivy::make_error_code(IVY_EINVAL));
    assert(!bus.bind_raw(ignore, ivy::runtime_regexp(std::string_view("a\0b", 3))));
    assert(!bus.bind_raw(ivy::Bus::MessageCallback{}, "^regexp"));
    assert(!bus.bind_direct(ivy::Bus::DirectCallback{}));
    assert(ctx->bindings.size() == count);

    bind_error = IVY_ENOMEM;
    auto failed = bus.bind_raw(ignore, "^failed");
    bind_error = IVY_OK;
    assert(!failed && failed.error() == ivy::make_error_code(IVY_ENOMEM));
    assert(ctx->bindings.size() == count);
    ivy::Bus moved(std::move(bus));
    assert(result->is_bound());
    auto from_moved = bus.bind_raw(ignore, "^failed");
    assert(!from_moved && from_moved.error() == ivy::make_error_code(IVY_ESTATE));

    const int before = unbind_count;
    ivy::Subscription subscription(std::move(*result));
    assert(!result->is_bound());
    assert(subscription.unbind());
    assert(subscription.unbind());
    assert(unbind_count == before + 1);
    pending_callback(&peer, pending_data, 1, arguments);
    assert(calls == 2); // C may invoke a callback copied before unbind.
    assert(moved.stop());
    assert(!formatted->is_bound());
    assert(formatted->unbind());
    auto after_stop = moved.bind_raw(ignore, "^failed");
    assert(!after_stop && after_stop.error() == ivy::make_error_code(IVY_ESTOPPED));
}

void raw_callbacks_without_sender() {
    auto bus = require_bus(ivy::Bus::create("raw captures"));
    auto* ctx = bus.native_handle();
    _clnt_lst_dict peer;
    char value[] = "42";
    char* arguments[] = {value};
    int calls = 0;
    auto make_callback = [&] {
        return [count = std::make_unique<int>(0), &calls, &value](auto args) mutable noexcept {
            if (++*count == 1) {
                assert(args.size() == 1 && args[0] == "42" && args[0].data() == value);
            } else {
                assert(*count == 2 && args.empty());
            }
            ++calls;
        };
    };
    std::array subscriptions{
        bus.bind_raw(make_callback(), "^VALUE (.*)$"),
        bus.bind_raw(make_callback(), ivy::runtime_regexp("^VALUE (.*)$")),
        bus.bind_raw(make_callback(), "^VALUE {} (.*)$", 7),
        bus.bind_raw_unanchored(make_callback(), "VALUE (.*)$"),
        bus.bind_raw_unanchored(make_callback(), "VALUE {} (.*)$", 7),
    };
    assert(ctx->bindings.size() == subscriptions.size());
    for (std::size_t i = 0; i < subscriptions.size(); ++i) {
        assert(subscriptions[i] && subscriptions[i]->is_bound());
        auto* binding = ctx->bindings[i];
        binding->callback(&peer, binding->data, 1, arguments);
        binding->callback(&peer, binding->data, 0, nullptr);
    }
    assert(calls == 10);

    // A callable accepting both forms must continue to receive the sender.
    bool received_peer = false;
    auto generic = bus.bind_raw([&](auto... args) {
        if constexpr (sizeof...(args) == 2)
            received_peer = std::get<0>(std::tuple(args...)) == &peer;
        else
            assert(false);
    }, "^GENERIC$");
    assert(generic);
    auto* binding = ctx->bindings.back();
    binding->callback(&peer, binding->data, 0, nullptr);
    assert(received_peer);

    const auto count = ctx->bindings.size();
    using Captures = std::span<const std::string_view>;
    expect_error(IVY_EINVAL, bus.bind_raw(std::function<void(Captures)>{}, "^EMPTY$"));
    expect_error(IVY_EINVAL, bus.bind_raw(std::move_only_function<void(Captures)>{},
        ivy::runtime_regexp("^EMPTY$")));
    expect_error(IVY_EINVAL, bus.bind_raw(static_cast<void(*)(Captures)>(nullptr), "^EMPTY {}$", 7));
    expect_error(IVY_EINVAL, bus.bind_raw_unanchored(std::function<void(Captures)>{}, "EMPTY$"));
    expect_error(IVY_EINVAL, bus.bind_raw_unanchored(std::move_only_function<void(Captures)>{}, "EMPTY {}$", 7));
    assert(ctx->bindings.size() == count);

    auto throwing = bus.bind_raw([](Captures) { throw 42; }, "^THROW$");
    assert(throwing);
    binding = ctx->bindings.back();
    binding->callback(&peer, binding->data, 0, nullptr);
    expect_error(ivy::Error::callback_failed, bus.take_callback_error());
    assert(bus.state() == IVY_CTX_STOPPED);
}

void direct_subscriptions() {
    auto bus = require_bus(ivy::Bus::create("direct"));
    auto* ctx = bus.native_handle();
    _clnt_lst_dict peer;
    int old_calls = 0;
    int new_calls = 0;
    auto old = bus.bind_direct([value = std::make_unique<int>(7), &old_calls, &peer]
                       (IvyClientPtr app, int id, std::string_view message) {
        assert(app == &peer && id == *value && message == "payload");
        ++old_calls;
    });
    assert(old);
    const auto old_callback = ctx->direct;
    void* old_data = ctx->direct_data;
    char message[] = "payload";
    old_callback(&peer, old_data, 7, message);
    assert(old_calls == 1);

    auto replacement = bus.bind_direct([&](IvyClientPtr, int id, std::string_view text) {
        assert(id == 8 && text == "payload");
        ++new_calls;
    });
    assert(replacement && replacement->is_bound() && !old->is_bound());
    old_callback(&peer, old_data, 7, message);
    assert(old_calls == 1);
    assert(old->unbind());
    ctx->direct(&peer, ctx->direct_data, 8, message);
    assert(new_calls == 1);
    bind_error = IVY_EIO;
    auto failed = bus.bind_direct([](IvyClientPtr, int, std::string_view) {});
    bind_error = IVY_OK;
    assert(!failed && replacement->is_bound());
    ctx->direct(&peer, ctx->direct_data, 8, message);
    assert(new_calls == 2);
    assert(replacement->unbind());
    assert(!ctx->direct);
}

void subscription_lifetimes() {
    ivy::Subscription survivor;
    std::weak_ptr<int> weak_capture;
    assert(!survivor.is_bound() && survivor.unbind());
    {
        auto bus = require_bus(ivy::Bus::create("lifetimes"));
        auto capture = std::make_shared<int>(42);
        weak_capture = capture;
        auto result = bus.bind_raw([capture = std::move(capture)](IvyClientPtr, auto) {}, "^first");
        assert(result);
        survivor = std::move(*result);
        const int before = unbind_count;
        {
            auto scoped = bus.bind_raw([](IvyClientPtr, auto) {}, "^scoped");
            assert(scoped);
        }
        assert(unbind_count == before + 1);
        assert(!weak_capture.expired());
    }
    assert(!survivor.is_bound() && survivor.unbind());
    assert(weak_capture.expired());

    auto bus = require_bus(ivy::Bus::create("self-unbind"));
    std::optional<ivy::Subscription> self;
    auto capture = std::make_shared<int>(42);
    weak_capture = capture;
    auto result = bus.bind_raw([capture = std::move(capture), &self, &weak_capture](IvyClientPtr, auto) {
        self.reset();
        assert(!weak_capture.expired() && *capture == 42);
    }, "^self");
    assert(result);
    self.emplace(std::move(*result));
    auto* binding = bus.native_handle()->bindings.back();
    const auto callback = binding->callback;
    void* data = binding->data;
    callback(nullptr, data, 0, nullptr);
    assert(!self && weak_capture.expired());
    callback(nullptr, data, 0, nullptr);
}

void concurrent_unbind() {
    auto bus = require_bus(ivy::Bus::create("concurrent unbind"));
    std::latch entered(1), release(1);
    auto capture = std::make_shared<int>(42);
    std::weak_ptr<int> weak_capture = capture;
    auto result = bus.bind_raw([capture = std::move(capture), &entered, &release](IvyClientPtr, auto) {
        entered.count_down();
        release.wait();
        assert(*capture == 42);
    }, "^blocked");
    assert(result);
    const auto callback = bus.native_handle()->bindings.back()->callback;
    void* data = bus.native_handle()->bindings.back()->data;
    std::thread worker([&] { callback(nullptr, data, 0, nullptr); });
    entered.wait();
    assert(result->unbind());
    assert(!result->is_bound() && !weak_capture.expired());
    release.count_down();
    worker.join();
    assert(weak_capture.expired());
    callback(nullptr, data, 0, nullptr);
}

void subscription_exceptions() {
    auto messages = require_bus(ivy::Bus::create("message exception"));
    auto subscription = messages.bind_raw([](IvyClientPtr, auto) { throw 41; }, "^exception");
    assert(subscription);
    auto* binding = messages.native_handle()->bindings.back();
    binding->callback(nullptr, binding->data, 0, nullptr);
    assert(messages.state() == IVY_CTX_STOPPED);
    expect_error(ivy::Error::callback_failed, messages.take_callback_error());

    auto direct = require_bus(ivy::Bus::create("direct exception"));
    auto direct_subscription = direct.bind_direct([](IvyClientPtr, int, std::string_view) { throw 42; });
    assert(direct_subscription);
    direct.native_handle()->direct(nullptr, direct.native_handle()->direct_data, 0, nullptr);
    assert(direct.state() == IVY_CTX_STOPPED);
    expect_error(ivy::Error::callback_failed, direct.take_callback_error());
}

void dispatch_captures(MsgRcvPtr binding, std::initializer_list<std::string> captures) {
    std::vector<std::string> values(captures);
    std::vector<char*> arguments;
    for (auto& value : values) arguments.push_back(value.data());
    binding->callback(nullptr, binding->data, static_cast<int>(arguments.size()), arguments.data());
}

template<class T>
void accepts_capture(std::string text, T expected) {
    auto bus = require_bus(ivy::Bus::create("valid conversion"));
    std::optional<T> received;
    auto subscription = bus.bind_convert([&](ivy::ConvertStatus status, T value) {
        assert(status == ivy::ConvertStatus::OK && bus.conversion_error().empty());
        received = value;
    }, "^VALUE (.*)$");
    assert(subscription);
    dispatch_captures(bus.native_handle()->bindings.back(), {text});
    assert(received && *received == expected && bus.take_callback_error());
}

template<class T>
void rejects_captures(std::initializer_list<std::string> texts) {
    for (const auto& text : texts) {
        auto bus = require_bus(ivy::Bus::create("invalid conversion"));
        assert(bus.start());
        bool called = false;
        auto subscription = bus.bind_convert([&](ivy::ConvertStatus status, T value) {
            assert(status == ivy::ConvertStatus::CONVERT_ERROR && value == T{});
            const auto error = bus.conversion_error();
            assert(error.starts_with("capture 1: cannot convert "));
            assert(error.find(std::format("\"{}\"", text)) != std::string_view::npos);
            assert(bus.conversion_error() == error);
            called = true;
        }, "^VALUE (.*)$");
        assert(subscription);
        dispatch_captures(bus.native_handle()->bindings.back(), {text});
        assert(called && bus.state() == IVY_CTX_RUNNING && subscription->is_bound());
        assert(bus.take_callback_error() && bus.conversion_error().empty());
    }
}

void converted_subscriptions() {
    accepts_capture("-42", -42L);
    accepts_capture("+42", 42L);
    accepts_capture("00042", 42L);
    accepts_capture("-0", 0L);
    accepts_capture(std::to_string(std::numeric_limits<long>::min()), std::numeric_limits<long>::min());
    accepts_capture(std::to_string(std::numeric_limits<long>::max()), std::numeric_limits<long>::max());
    rejects_captures<long>({"", "+", "-", "+-1", "++1", " 42", "42 ", "42x", "1.5", "1e2", "0x10",
        std::to_string(std::numeric_limits<long>::min()) + "0",
        std::to_string(std::numeric_limits<long>::max()) + "0"});
    accepts_capture("+1.25e2", 125.0);
    accepts_capture("-2.5E-1", -0.25);
    accepts_capture(".5", 0.5);
    accepts_capture("42", 42.0);
    accepts_capture("-0.0", -0.0);
    accepts_capture(std::format("{}", std::numeric_limits<double>::max()), std::numeric_limits<double>::max());
    accepts_capture(std::format("{}", std::numeric_limits<double>::min()), std::numeric_limits<double>::min());
    accepts_capture(std::format("{}", std::numeric_limits<double>::denorm_min()), std::numeric_limits<double>::denorm_min());
    rejects_captures<double>({"", "+", "-", "+-1", "++1", " 1.5", "1.5 ", "1.5x", "1,5", "1e",
        "0x1p2", "1e9999", "1e-9999", "nan", "NaN", "nan(1)", "inf", "-inf", "+inf", "infinity"});
    accepts_capture("true", true);
    accepts_capture("false", false);
    accepts_capture("1", true);
    accepts_capture("0", false);
    for (const char* text : {"2", "-1", "+1", "01", "-0001", "+0002",
                            "99999999999999999999999999999999999999999",
                            "-99999999999999999999999999999999999999999",
                            "TRUE", "true", "t", "T", "v", "V", "vrai", "Vrai", "true suffix", "t0"})
        accepts_capture(text, true);
    for (const char* text : {"+0", "-0", "000", "-000", "+000", "00000000000000000000000000000000000",
                            "False", "FALSE", "f", "F", "faux", "false ", "F1"})
        accepts_capture(text, false);
    rejects_captures<bool>({"", "yes", "no", " true", " 1", "1 ", "0.0", "1e2", "1x", "0x1", "+", "-", "+-1"});

    auto bus = require_bus(ivy::Bus::create("typed subscription"));
    _clnt_lst_dict peer;
    int calls = 0;
    char id[] = "-42", altitude[] = "+125.5", label[] = "name with spaces", active[] = "true";
    char* arguments[] = {id, altitude, label, active};
    auto subscription = bus.bind_convert(
        [count = std::make_unique<int>(0), &calls, &peer, &label]
        (IvyClientPtr sender, ivy::ConvertStatus status, long number, double height, std::string_view name, bool enabled) mutable {
            assert(status == ivy::ConvertStatus::OK);
            assert(sender == &peer && number == -42 && height == 125.5 && enabled);
            assert(name == "name with spaces" && name.data() == label);
            calls = ++*count;
        }, R"(^TRACK ([^ ]+) ([^ ]+) (.*) ([^ ]+)$)");
    assert(subscription && subscription->is_bound());
    auto* binding = bus.native_handle()->bindings.back();
    binding->callback(&peer, binding->data, 4, arguments);
    assert(calls == 1);
    assert(subscription->change("^UPDATED (.*) (.*) (.*) (.*)$"));
    auto moved_bus = std::move(bus);
    auto moved_subscription = std::move(*subscription);
    binding->callback(&peer, binding->data, 4, arguments);
    assert(calls == 2 && moved_subscription.is_bound() && !subscription->is_bound());
    assert(moved_subscription.unbind());

    auto dynamic = moved_bus.bind_convert([](ivy::ConvertStatus status, long value) {
        assert(status == ivy::ConvertStatus::OK && value == 42);
    },
        ivy::runtime_regexp(std::string("^DYNAMIC (.*)$")));
    assert(dynamic);
    dispatch_captures(moved_bus.native_handle()->bindings.back(), {"42"});
    auto formatted = moved_bus.bind_convert([](ivy::ConvertStatus status, long value) {
        assert(status == ivy::ConvertStatus::OK && value == 42);
    },
        R"(^TRACK {} ([0-9]{{2}}) 100%$)", 7);
    assert(formatted && moved_bus.native_handle()->bindings.back()->regexp == R"(^TRACK 7 ([0-9]{2}) 100%$)");
    dispatch_captures(moved_bus.native_handle()->bindings.back(), {"42"});

    int empty_calls = 0;
    auto empty = moved_bus.bind_convert([&](ivy::ConvertStatus status) noexcept {
        assert(status == ivy::ConvertStatus::OK); ++empty_calls;
    }, "^EMPTY$");
    assert(empty);
    dispatch_captures(moved_bus.native_handle()->bindings.back(), {});
    auto peer_only = moved_bus.bind_convert([&](IvyClientPtr sender, ivy::ConvertStatus status) {
        assert(sender == &peer && status == ivy::ConvertStatus::OK); ++empty_calls;
    }, "^PEER$");
    assert(peer_only);
    auto* peer_binding = moved_bus.native_handle()->bindings.back();
    peer_binding->callback(&peer, peer_binding->data, 0, nullptr);
    auto empty_view = moved_bus.bind_convert([&](ivy::ConvertStatus status, std::string_view text) {
        assert(status == ivy::ConvertStatus::OK && text.empty()); ++empty_calls;
    }, "^TEXT (.*)$");
    assert(empty_view);
    dispatch_captures(moved_bus.native_handle()->bindings.back(), {""});
    assert(empty_calls == 3);

    for (auto captures : {std::initializer_list<std::string>{}, {"42", "extra"}}) {
        auto mismatch = require_bus(ivy::Bus::create("capture count"));
        int count_calls = 0;
        auto typed = mismatch.bind_convert([&](IvyClientPtr sender, ivy::ConvertStatus status, long value) {
            assert(sender == &peer && status == ivy::ConvertStatus::COUNT_ERROR && value == 0);
            assert(mismatch.conversion_error() == std::format(
                "capture count mismatch: expected 1, received {}", captures.size()));
            ++count_calls;
        }, "^ONE (.*)$");
        assert(typed && typed->change("^CHANGED$"));
        auto* mismatch_binding = mismatch.native_handle()->bindings.back();
        mismatch_binding->callback(&peer, mismatch_binding->data, static_cast<int>(captures.size()), arguments);
        assert(count_calls == 1 && mismatch.take_callback_error() && typed->is_bound());
    }
    // Discard all partial conversions, preserving the sender and reporting the first failure.
    auto partial = require_bus(ivy::Bus::create("partial conversion"));
    int partial_calls = 0;
    auto mixed = partial.bind_convert([&](IvyClientPtr sender, ivy::ConvertStatus status,
                                          long id, double altitude, std::string_view name, bool enabled) {
        ++partial_calls;
        assert(status == ivy::ConvertStatus::CONVERT_ERROR && sender == &peer);
        assert(id == 0 && altitude == 0.0 && name.empty() && !enabled);
        assert(partial.conversion_error() ==
            "capture 4: cannot convert \"bad\" to bool: expected an integer or a value starting with f/F, t/T or v/V");
    }, "^MIXED (.*) (.*) (.*) (.*)$");
    assert(mixed);
    char bad_bool[] = "bad";
    arguments[3] = bad_bool;
    auto* mixed_binding = partial.native_handle()->bindings.back();
    mixed_binding->callback(&peer, mixed_binding->data, 4, arguments);
    assert(partial_calls == 1 && partial.take_callback_error() && partial.conversion_error().empty());

    auto throwing = require_bus(ivy::Bus::create("typed callback exception"));
    auto failed = throwing.bind_convert([](ivy::ConvertStatus, long) { throw 42; }, "^THROW (.*)$");
    assert(failed);
    dispatch_captures(throwing.native_handle()->bindings.back(), {"42"});
    expect_error(ivy::Error::callback_failed, throwing.take_callback_error());

    expect_error(IVY_EINVAL, moved_bus.bind_convert(std::function<void(ivy::ConvertStatus, long)>{}, "^EMPTY"));
    expect_error(IVY_EINVAL, moved_bus.bind_convert(std::move_only_function<void(ivy::ConvertStatus, long)>{}, "^EMPTY"));
    expect_error(IVY_EINVAL, moved_bus.bind_convert(static_cast<void(*)(ivy::ConvertStatus, long)>(nullptr), "^EMPTY"));
    expect_error(IVY_EINVAL, moved_bus.bind_convert([](ivy::ConvertStatus, long) {}, ivy::runtime_regexp(std::string_view("^A\0B", 4))));
    expect_error(IVY_ESTATE, bus.bind_convert([](ivy::ConvertStatus, long) {}, "^MOVED"));
    assert(moved_bus.stop());
    expect_error(IVY_ESTOPPED, moved_bus.bind_convert([](ivy::ConvertStatus, long) {}, "^STOPPED"));
}

void conversion_diagnostics() {
    auto original = require_bus(ivy::Bus::create("conversion diagnostics"));
    ivy::Bus* current_bus = &original;
    std::vector<ivy::ConvertStatus> statuses;
    std::vector<std::string> errors;
    auto subscription = original.bind_convert([&](ivy::ConvertStatus status, double value) {
        statuses.push_back(status);
        errors.emplace_back(current_bus->conversion_error());
        assert(value == (status == ivy::ConvertStatus::OK ? 42.5 : 0.0));
    }, "^VALUE (.*)$");
    assert(subscription && original.start());
    auto bus = std::move(original);
    current_bus = &bus;
    auto* binding = bus.native_handle()->bindings.back();
    for (const char* text : {"", "bad", "1.5x", "1e9999", "nan", "42.5"})
        dispatch_captures(binding, {text});
    assert(statuses == std::vector<ivy::ConvertStatus>({
        ivy::ConvertStatus::CONVERT_ERROR, ivy::ConvertStatus::CONVERT_ERROR,
        ivy::ConvertStatus::CONVERT_ERROR, ivy::ConvertStatus::CONVERT_ERROR,
        ivy::ConvertStatus::CONVERT_ERROR, ivy::ConvertStatus::OK}));
    assert(errors == std::vector<std::string>({
        "capture 1: cannot convert \"\" to double: empty numeric capture",
        "capture 1: cannot convert \"bad\" to double: invalid numeric syntax",
        "capture 1: cannot convert \"1.5x\" to double: trailing characters in numeric capture",
        "capture 1: cannot convert \"1e9999\" to double: numeric value out of range",
        "capture 1: cannot convert \"nan\" to double: expected a finite number", ""}));
    assert(bus.state() == IVY_CTX_RUNNING && subscription->is_bound() && bus.take_callback_error());
    assert(bus.conversion_error().empty() && original.conversion_error().empty());

    // Two bad captures still report the first one and default all values.
    auto first = bus.bind_convert([&](ivy::ConvertStatus status, long id, bool active) {
        assert(status == ivy::ConvertStatus::CONVERT_ERROR && id == 0 && !active);
        assert(bus.conversion_error() == "capture 1: cannot convert \"bad\" to long: invalid numeric syntax");
    }, "^FIRST (.*) (.*)$");
    assert(first);
    dispatch_captures(bus.native_handle()->bindings.back(), {"bad", "also bad"});

    auto other = require_bus(ivy::Bus::create("other conversion bus"));
    std::string_view outer_view;
    std::string outer_copy;
    auto nested = bus.bind_convert([&](ivy::ConvertStatus status, long) {
        assert(status == ivy::ConvertStatus::COUNT_ERROR);
        assert(bus.conversion_error() == "capture count mismatch: expected 1, received 0");
        assert(outer_view == outer_copy);
    }, "^NESTED (.*)$");
    assert(nested);
    auto* nested_binding = bus.native_handle()->bindings.back();
    auto nested_ok = bus.bind_convert([&](ivy::ConvertStatus status) {
        assert(status == ivy::ConvertStatus::OK && bus.conversion_error().empty());
        assert(outer_view == outer_copy);
    }, "^NESTED_OK$");
    assert(nested_ok);
    auto* nested_ok_binding = bus.native_handle()->bindings.back();
    auto other_subscription = other.bind_convert([&](ivy::ConvertStatus status, bool) {
        assert(status == ivy::ConvertStatus::CONVERT_ERROR);
        assert(other.conversion_error() ==
            "capture 1: cannot convert \"no\" to bool: expected an integer or a value starting with f/F, t/T or v/V");
        assert(bus.conversion_error() == outer_copy);
    }, "^OTHER (.*)$");
    assert(other_subscription);
    auto* other_binding = other.native_handle()->bindings.back();
    auto outer = bus.bind_convert([&](ivy::ConvertStatus status, long) {
        assert(status == ivy::ConvertStatus::CONVERT_ERROR && other.conversion_error().empty());
        outer_view = bus.conversion_error();
        outer_copy = outer_view;
        dispatch_captures(nested_binding, {});
        dispatch_captures(nested_ok_binding, {});
        dispatch_captures(other_binding, {"no"});
        assert(bus.conversion_error() == outer_copy && outer_view == outer_copy);
        assert(other.conversion_error().empty());
    }, "^OUTER (.*)$");
    assert(outer);
    dispatch_captures(bus.native_handle()->bindings.back(), {"outer failure"});
    assert(bus.conversion_error().empty() && other.conversion_error().empty());

    // Exceptions restore the diagnostic context before the existing C boundary handles them.
    auto throwing = require_bus(ivy::Bus::create("conversion context unwind"));
    auto throws = throwing.bind_convert([&](ivy::ConvertStatus status, long) {
        assert(status == ivy::ConvertStatus::CONVERT_ERROR && !throwing.conversion_error().empty());
        throw 42;
    }, "^THROW (.*)$");
    assert(throws);
    dispatch_captures(throwing.native_handle()->bindings.back(), {"bad"});
    assert(throwing.conversion_error().empty());
    expect_error(ivy::Error::callback_failed, throwing.take_callback_error());
    static_assert(noexcept(bus.conversion_error()));
}

void concurrent_conversion_diagnostics() {
    auto bus = require_bus(ivy::Bus::create("concurrent conversion diagnostics"));
    std::latch entered(2), release(1);
    auto make_callback = [&](std::string expected) {
        return [&, expected = std::move(expected)](ivy::ConvertStatus status, long) {
            assert(status == ivy::ConvertStatus::CONVERT_ERROR);
            const auto error = bus.conversion_error();
            assert(error == expected);
            entered.count_down();
            release.wait();
            assert(error == expected && bus.conversion_error() == expected);
        };
    };
    auto first = bus.bind_convert(make_callback(
        "capture 1: cannot convert \"one\" to long: invalid numeric syntax"), "^ONE (.*)$");
    assert(first);
    auto* first_binding = bus.native_handle()->bindings.back();
    auto second = bus.bind_convert(make_callback(
        "capture 1: cannot convert \"two\" to long: invalid numeric syntax"), "^TWO (.*)$");
    assert(second);
    auto* second_binding = bus.native_handle()->bindings.back();
    std::thread one([&] { dispatch_captures(first_binding, {"one"}); });
    std::thread two([&] { dispatch_captures(second_binding, {"two"}); });
    entered.wait();
    assert(bus.conversion_error().empty());
    release.count_down();
    one.join();
    two.join();
    assert(bus.take_callback_error());
}

void change_preserves_subscription() {
    ivy::Subscription empty;
    expect_error(IVY_ESTATE, empty.change("^pattern"));
    ivy::Subscription survivor;
    {
        auto bus = require_bus(ivy::Bus::create("change"));
        int calls = 0;
        auto result = bus.bind_raw([count = std::make_unique<int>(0), &calls](IvyClientPtr, auto) {
            calls = ++*count;
        }, "^OLD$");
        assert(result);
        auto* ctx = bus.native_handle();
        auto* original = ctx->bindings.back();
        const auto callback = original->callback;
        void* data = original->data;
        callback(nullptr, data, 0, nullptr);
        assert(calls == 1);

        const std::string pattern = R"(^NEW [0-9]{2} 100%$-suffix)";
        assert(result->change(ivy::runtime_regexp(std::string_view(pattern).substr(0, pattern.size() - 7))));
        assert(ctx->bindings.size() == 1 && ctx->bindings.back() == original);
        assert(original->regexp == R"(^NEW [0-9]{2} 100%$)");
        assert(original->callback == callback && original->data == data);
        callback(nullptr, data, 0, nullptr);
        assert(calls == 2);

        assert(result->change(R"(^NEW {} [0-9]{{2}} 100%$)", 42));
        const auto previous_pattern = original->regexp;
        assert(previous_pattern == R"(^NEW 42 [0-9]{2} 100%$)");
        change_error = IVY_ENOMEM;
        expect_error(IVY_ENOMEM, result->change("^failed"));
        change_error = IVY_OK;
        expect_error(IVY_EINVAL, result->change(ivy::runtime_regexp(std::string_view("a\0b", 3))));
        expect_error(IVY_EINVAL, result->change("^{:{}d}", 42, -1));
        assert(original->regexp == previous_pattern && result->is_bound());
        callback(nullptr, data, 0, nullptr);
        assert(calls == 3);

        survivor = std::move(*result);
        expect_error(IVY_ESTATE, result->change("^moved-from"));
        assert(survivor.change("^MOVED$"));
        assert(bus.stop());
        expect_error(IVY_ESTOPPED, survivor.change("^stopped"));
    }
    expect_error(IVY_ESTATE, survivor.change("^destroyed bus"));
    assert(!survivor.is_bound() && survivor.unbind());
}

void change_and_unbind() {
    auto bus = require_bus(ivy::Bus::create("change and unbind"));
    auto result = bus.bind_raw([](IvyClientPtr, auto) {}, "^initial");
    assert(result);
    const int before = unbind_count;
    during_change = [&] {
        assert(result->unbind());
        assert(!result->is_bound());
        assert(unbind_count == before && bus.native_handle()->bindings.size() == 1);
    };
    expect_error(IVY_ESTATE, result->change("^cancelled reentrantly"));
    during_change = nullptr;
    assert(unbind_count == before + 1 && bus.native_handle()->bindings.empty());
    expect_error(IVY_ESTATE, result->change("^unbound"));
    assert(result->unbind());

    auto concurrent = bus.bind_raw([](IvyClientPtr, auto) {}, "^concurrent");
    assert(concurrent);
    std::latch entered(1), release(1);
    during_change = [&] { entered.count_down(); release.wait(); };
    std::expected<void, std::error_code> changed;
    std::thread worker([&] { changed = concurrent->change("^while unbinding"); });
    entered.wait();
    assert(concurrent->unbind() && !concurrent->is_bound());
    assert(bus.native_handle()->bindings.size() == 1);
    release.count_down();
    worker.join();
    during_change = nullptr;
    expect_error(IVY_ESTATE, changed);
    assert(bus.native_handle()->bindings.empty());

    // Destruction from a C callback dispatched by change must also defer removal.
    std::optional<ivy::Subscription> self;
    auto self_result = bus.bind_raw([](IvyClientPtr, auto) {}, "^self");
    assert(self_result);
    self.emplace(std::move(*self_result));
    during_change = [&] { self.reset(); };
    expect_error(IVY_ESTATE, self->change("^destroyed during change"));
    during_change = nullptr;
    assert(!self && bus.native_handle()->bindings.empty());
}

void direct_token_lifetimes() {
    ivy::DirectSubscription survivor;
    assert(!survivor.is_bound() && survivor.unbind());
    std::weak_ptr<int> weak_capture;
    {
        auto first = require_bus(ivy::Bus::create("first direct"));
        auto second = require_bus(ivy::Bus::create("second direct"));
        auto capture = std::make_shared<int>(42);
        weak_capture = capture;
        auto a = first.bind_direct([](IvyClientPtr, int, std::string_view) {});
        auto b = second.bind_direct([capture = std::move(capture)](IvyClientPtr, int, std::string_view) {});
        assert(a && b);
        *a = std::move(*b);
        assert(!b->is_bound() && a->is_bound());
        assert(!first.native_handle()->direct && second.native_handle()->direct);
        survivor = std::move(*a);
        assert(!a->is_bound() && survivor.is_bound());
    }
    assert(!survivor.is_bound() && survivor.unbind());
    assert(weak_capture.expired());
}

void anchoring_boundary() {
    auto bus = require_bus(ivy::Bus::create("anchoring policy"));
    auto callback = [](IvyClientPtr, auto) {};
    auto result = bus.bind_raw(callback, "^PREFIX {}", 42);
    assert(result && validated_regexp == "^PREFIX 42");
    assert(result->change("^CHANGED {}", 43));
    assert(validated_regexp == "^CHANGED 43");

    std::string dynamic = "^DYNAMIC [0-9]{2}";
    assert(bus.bind_raw(callback, ivy::runtime_regexp(dynamic)));
    assert(validated_regexp == dynamic);
    auto missing_anchor = bus.bind_raw(callback, ivy::runtime_regexp("DYNAMIC"));
    assert(!missing_anchor && missing_anchor.error() == ivy::make_error_code(IVY_EUNANCHORED));

    validation_error = IVY_ENOMEM;
    auto failed = bus.bind_raw(callback, "^REJECTED");
    assert(!failed && failed.error() == ivy::make_error_code(IVY_ENOMEM));
    expect_error(IVY_ENOMEM, result->change("^REJECTED"));
    assert(bus.native_handle()->bindings.front()->regexp == "^CHANGED 43");
    validation_error = IVY_OK;

    const int before = validations;
    auto unanchored = bus.bind_raw_unanchored(callback, "ANYWHERE {}", 44);
    assert(unanchored && validations == before);
    assert(unanchored->change_unanchored("ELSEWHERE {}", 45));
    assert(validations == before);
    assert(bus.native_handle()->bindings.back()->regexp == "ELSEWHERE 45");
    assert(unanchored->change("^ANCHORED AGAIN"));
    assert(validations == before + 1);
}

template<class T>
concept HasChange = requires(T& subscription) { subscription.change("^regexp"); };

void sending_and_transport_callback() {
    auto bus = require_bus(ivy::Bus::create("send"));
    _clnt_lst_dict peer;
    auto before_start = bus.send("text");
    assert(!before_start && before_start.error() == ivy::make_error_code(IVY_ESTATE));
    assert(bus.start());
    next_send_report = {3, 3, 0, 0};
    auto sent = bus.send("100% é UTF-8 {} unchanged");
    assert(sent && *sent == 3 && sent_message == "100% é UTF-8 {} unchanged");
    assert(bus.send("TRACK {} {:.2f}", 42, 1.25));
    assert(sent_message == "TRACK 42 1.25");
    const std::string with_suffix = "message-suffix";
    assert(bus.send(std::string_view(with_suffix).substr(0, 7)));
    assert(sent_message == "message");
    assert(bus.send(&peer, 7, "DIRECT {}", 42));
    assert(sent_peer == &peer && sent_id == 7 && sent_message == "DIRECT 42");
    for (char invalid : {'\0', '\n', '\002', '\003'}) {
        std::string message = "text";
        message.push_back(invalid);
        assert(!bus.send(message));
        assert(!bus.send(&peer, 7, message));
    }
    assert(!bus.send("{:{}d}", 1, -1));
    const auto invalid_format = bus.send_report("{:{}d}", 1, -1);
    assert(invalid_format.error == ivy::make_error_code(IVY_EINVAL));
    assert(!invalid_format.matched && !invalid_format.accepted && !invalid_format.failed);
    assert(!bus.send(nullptr, 7, "direct"));
    next_send_report = {};
    sent = bus.send("");
    assert(sent && *sent == 0);

    send_error = IVY_EIO;
    next_send_report = {3, 2, 1, EPIPE};
    assert(!bus.send("partial"));
    const auto report = bus.send_report("partial {}", 42);
    assert(sent_message == "partial 42");
    assert(report.matched == 3 && report.accepted == 2 && report.failed == 1);
    assert(report.error == ivy::make_error_code(IVY_EIO));
    assert(report.system_error.value() == EPIPE);
    send_error = IVY_EFIFOFULL;
    auto direct_error = bus.send(&peer, 7, "full");
    assert(!direct_error && direct_error.error() == ivy::make_error_code(IVY_EFIFOFULL));
    send_error = IVY_OK;
    next_send_report = {};

    int notifications = 0;
    assert(bus.set_transport_error_callback(
        [value = std::make_unique<int>(42), &notifications, &bus]
        (IvyClientPtr, std::error_code error, int system_error) {
            assert(*value == 42 && error == ivy::make_error_code(IVY_EIO) && system_error == EPIPE);
            assert(bus.set_transport_error_callback({}));
            ++notifications;
        }));
    auto* context = bus.native_handle();
    context->transport(&peer, context->transport_data, IVY_EIO, EPIPE);
    context->transport(&peer, context->transport_data, IVY_EIO, EPIPE);
    assert(notifications == 1);
    assert(bus.set_transport_error_callback([](IvyClientPtr, std::error_code, int) { throw 99; }));
    context->transport(&peer, context->transport_data, IVY_EIO, EPIPE);
    assert(bus.state() == IVY_CTX_STOPPED);
    expect_error(ivy::Error::callback_failed, bus.take_callback_error());
    assert(!bus.send("stopped"));
    assert(!bus.set_transport_error_callback({}));
}

// Fail inside user-defined conversion/formatting code. The wrapper must return
// errors even when callback storage is constructed from a lambda or functor.
struct FailingCallback {
    bool allocation;
    explicit FailingCallback(bool allocation_failure) noexcept : allocation(allocation_failure) {}
    FailingCallback(const FailingCallback& other) : allocation(other.allocation) {
        if (allocation) throw std::bad_alloc();
        throw 123;
    }
    template<class... Args> void operator()(Args&&...) const {}
};

struct FailingFormat { int kind; };

struct FailingTypedCallback : FailingCallback {
    using FailingCallback::FailingCallback;
    void operator()(ivy::ConvertStatus, long) const {}
};

struct FailingRawCallback : FailingCallback {
    using FailingCallback::FailingCallback;
    void operator()(std::span<const std::string_view>) const {}
};

template<> struct std::formatter<FailingFormat> {
    constexpr auto parse(std::format_parse_context& context) { return context.begin(); }
    auto format(const FailingFormat& value, std::format_context& context) const {
        switch (value.kind) {
        case 0: throw std::bad_alloc();
        case 1: throw std::format_error("format failure");
        case 2: throw std::length_error("length failure");
        default: throw 123;
        }
        return context.out();
    }
};

void nonthrowing_boundaries() {
    auto bus = require_bus(ivy::Bus::create("nonthrowing"));
    assert(bus.start());
    auto callback = [](IvyClientPtr, std::span<const std::string_view>) {};
    auto subscription = bus.bind_raw(callback, "^original");
    assert(subscription);
    _clnt_lst_dict peer;
    for (bool allocation : {false, true}) {
        FailingCallback failing(allocation);
        const auto expected = allocation ? ivy::make_error_code(IVY_ENOMEM)
            : ivy::make_error_code(ivy::Error::callback_failed);
        auto check = [&](auto result) { assert(!result && result.error() == expected); };
        check(ivy::Bus::create("bad application callback", std::nullopt, failing));
        check(ivy::Bus::create("bad die callback", std::nullopt, {}, failing));
        check(bus.bind_raw(failing, "^regexp"));
        check(bus.bind_raw(failing, ivy::runtime_regexp("^regexp")));
        check(bus.bind_raw(failing, "^regexp {}", 42));
        check(bus.bind_raw_unanchored(failing, "regexp"));
        check(bus.bind_raw_unanchored(failing, "regexp {}", 42));
        FailingRawCallback raw(allocation);
        check(bus.bind_raw(raw, "^regexp"));
        check(bus.bind_raw(raw, ivy::runtime_regexp("^regexp")));
        check(bus.bind_raw(raw, "^regexp {}", 42));
        check(bus.bind_raw_unanchored(raw, "regexp"));
        check(bus.bind_raw_unanchored(raw, "regexp {}", 42));
        check(bus.bind_direct(failing));
        check(bus.bind_event(failing, ivy::pong));
        check(bus.bind_event(failing, ivy::remote_bindings));
        check(bus.bind_event(failing, ivy::every(std::chrono::milliseconds(10))));
        check(bus.bind_event(failing, ivy::every(std::chrono::milliseconds(10), 2)));
        check(bus.bind_event(failing, ivy::after(std::chrono::milliseconds(0))));
        check(bus.set_transport_error_callback(failing));
        FailingTypedCallback typed(allocation);
        check(bus.bind_convert(typed, "^TYPED (.*)$"));
        check(bus.bind_convert(typed, ivy::runtime_regexp("^TYPED (.*)$")));
        check(bus.bind_convert(typed, "^TYPED {} (.*)$", 42));
    }
    for (int kind : {0, 1, 2, 3}) {
        FailingFormat failing{kind};
        const auto expected = kind == 0 ? ivy::make_error_code(IVY_ENOMEM)
            : kind == 3 ? ivy::make_error_code(ivy::Error::formatter_failed)
            : ivy::make_error_code(IVY_EINVAL);
        auto check = [&](auto result) { assert(!result && result.error() == expected); };
        check(bus.send("{}", failing));
        check(bus.send(&peer, 1, "{}", failing));
        check(bus.send_error(&peer, 1, "{}", failing));
        check(ivy::validate_anchored_regexp("^{}", failing));
        const auto report = bus.send_report("{}", failing);
        assert(report.error == expected && !report.system_error);
        assert(report.matched == 0 && report.accepted == 0 && report.failed == 0);
        check(bus.bind_raw(callback, "^{}", failing));
        check(bus.bind_convert([](ivy::ConvertStatus, long) {}, "^{} (.*)$", failing));
        check(bus.bind_raw_unanchored(callback, "{}", failing));
        check(subscription->change("^{}", failing));
        check(subscription->change_unanchored("{}", failing));
        assert(subscription->is_bound());
        assert(bus.native_handle()->bindings.front()->regexp == "^original");
    }
    auto* binding = bus.native_handle()->bindings.front();
    binding->callback(nullptr, binding->data, 1, nullptr);
    expect_error(IVY_EINVAL, bus.take_callback_error());
    assert(bus.take_callback_error());

    static_assert(noexcept(ivy::Bus::create("app")));
    static_assert(noexcept(ivy::Bus::create("app", std::nullopt, FailingCallback(false))));
    static_assert(noexcept(bus.stop()));
    static_assert(noexcept(bus.take_callback_error()));
    constexpr std::format_string<int> format = "{}";
    static_assert(noexcept(bus.send(format, 42)));
    static_assert(noexcept(bus.send_report(format, 42)));
    static_assert(noexcept(bus.send(&peer, 1, format, 42)));
    static_assert(noexcept(bus.bind_raw(callback, "^regexp")));
    static_assert(noexcept(bus.bind_raw(callback, "^{}", 42)));
    static_assert(noexcept(subscription->change("^{}", 42)));
}

void timer_event(TimerId timer, unsigned long lateness = 0) {
    if (timer->removed) return;
    timer->owner->in_callback = true;
    timer->callback(timer, timer->data, lateness);
    timer->owner->in_callback = false;
}

void event_subscriptions_and_ping() {
    auto bus = require_bus(ivy::Bus::create("events"));
    auto* ctx = bus.native_handle();
    _clnt_lst_dict peer{ctx}, foreign{};
    int pong_calls = 0, binding_calls = 0;
    expect_error(IVY_ESTATE, bus.send_ping(&peer));
    auto pong = bus.bind_event([counter = std::make_unique<int>(0), &pong_calls, &peer]
        (IvyClientPtr app, int delay) {
            assert(app == &peer && (delay == 250 || delay == -1000));
            pong_calls = ++*counter;
        }, ivy::pong);
    auto bindings = bus.bind_event([&](IvyClientPtr app, int id, std::string_view pattern, IvyBindEvent event) {
        assert(app == &peer && id == 4 && pattern == "^REMOTE$");
        assert(event == IvyAddBind || event == IvyRemoveBind || event == IvyFilterBind || event == IvyChangeBind);
        ++binding_calls;
    }, ivy::remote_bindings);
    assert(pong && bindings && pong->is_bound() && bindings->is_bound());
    assert(bus.start());
    ctx->pong(&peer, ctx->pong_data, 250);
    ctx->pong(&peer, ctx->pong_data, -1000);
    for (auto event : {IvyAddBind, IvyRemoveBind, IvyFilterBind, IvyChangeBind})
        ctx->remote_bindings(&peer, ctx->remote_bindings_data, 4, "^REMOTE$", event);
    assert(pong_calls == 2 && binding_calls == 4);
    expect_error(IVY_EINVAL, bus.send_ping(nullptr));
    expect_error(IVY_EINVAL, bus.send_ping(&foreign));
    const int before = pings;
    assert(bus.send_ping(&peer) && pings == before + 1);
    ping_error = IVY_EIO;
    expect_error(IVY_EIO, bus.send_ping(&peer));
    ping_error = IVY_OK;
    event_registration_error = IVY_ENOMEM;
    expect_error(IVY_ENOMEM, bus.bind_event([](auto...) {}, ivy::pong));
    expect_error(IVY_ENOMEM, bus.bind_event([](auto...) {}, ivy::remote_bindings));
    event_registration_error = IVY_OK;
    assert(pong->is_bound() && bindings->is_bound());
    expect_error(IVY_EINVAL, bus.bind_event(ivy::Bus::PongCallback{}, ivy::pong));
    expect_error(IVY_EINVAL, bus.bind_event(ivy::Bus::RemoteBindingsCallback{}, ivy::remote_bindings));

    const auto old_pong = ctx->pong;
    void* old_pong_data = ctx->pong_data;
    auto replacement = bus.bind_event([](auto...) {}, ivy::pong);
    assert(replacement && !pong->is_bound() && bindings->is_bound());
    assert(pong->unbind());
    old_pong(&peer, old_pong_data, 250);
    assert(pong_calls == 2 && replacement->is_bound());
    const auto old_bind = ctx->remote_bindings;
    void* old_bind_data = ctx->remote_bindings_data;
    auto replacement_bind = bus.bind_event([](auto...) {}, ivy::remote_bindings);
    assert(replacement_bind && !bindings->is_bound() && bindings->unbind());
    old_bind(&peer, old_bind_data, 4, "^REMOTE$", IvyAddBind);
    assert(binding_calls == 4);

    // Native sending may synchronously call application code that cancels pong.
    during_ping = [&] { assert(replacement->unbind()); };
    expect_error(IVY_ESTATE, bus.send_ping(&peer));
    during_ping = {};
    assert(!ctx->pong && replacement_bind->is_bound());
    auto moved = std::move(bus);
    assert(replacement_bind->is_bound());
    expect_error(IVY_ESTATE, bus.bind_event([](auto...) {}, ivy::pong));
    expect_error(IVY_ESTATE, bus.bind_event([](auto...) {}, ivy::remote_bindings));
    expect_error(IVY_ESTATE, bus.send_ping(&peer));
    assert(moved.stop());
    assert(!replacement_bind->is_bound() && replacement_bind->unbind());
    expect_error(IVY_ESTOPPED, moved.bind_event([](auto...) {}, ivy::pong));
    expect_error(IVY_ESTOPPED, moved.bind_event([](auto...) {}, ivy::remote_bindings));
    expect_error(IVY_ESTOPPED, moved.send_ping(&peer));
}

void event_callback_lifetimes() {
    for (bool remote : {false, true}) {
        ivy::EventSubscription survivor;
        std::weak_ptr<int> weak;
        {
            auto bus = require_bus(ivy::Bus::create("event lifetimes"));
            auto register_event = [&](auto callback) {
                return remote ? bus.bind_event(std::move(callback), ivy::remote_bindings)
                              : bus.bind_event(std::move(callback), ivy::pong);
            };
            auto fire = [&] {
                auto* ctx = bus.native_handle();
                if (remote) ctx->remote_bindings(nullptr, ctx->remote_bindings_data, 0, nullptr, IvyAddBind);
                else ctx->pong(nullptr, ctx->pong_data, 0);
            };
            auto capture = std::make_shared<int>(42);
            weak = capture;
            std::latch entered(1), release(1);
            auto result = register_event([value = std::move(capture), &entered, &release](auto...) {
                entered.count_down();
                release.wait();
                assert(*value == 42);
            });
            assert(result);
            std::thread callback_thread(fire);
            entered.wait();
            assert(result->unbind() && !result->is_bound() && !weak.expired());
            release.count_down();
            callback_thread.join();
            assert(weak.expired());

            std::optional<ivy::EventSubscription> self;
            auto self_result = register_event([&self](auto...) { self.reset(); });
            assert(self_result);
            self.emplace(std::move(*self_result));
            fire();
            assert(!self);

            auto failed = register_event([](auto...) { throw std::bad_alloc(); });
            assert(failed);
            fire();
            assert(bus.state() == IVY_CTX_STOPPED);
            expect_error(IVY_ENOMEM, bus.take_callback_error());
            assert(bus.take_callback_error());
        }
        {
            auto bus = require_bus(ivy::Bus::create("surviving event"));
            auto capture = std::make_shared<int>(42);
            weak = capture;
            auto callback = [value = std::move(capture)](auto...) {};
            auto result = remote ? bus.bind_event(std::move(callback), ivy::remote_bindings)
                                 : bus.bind_event(std::move(callback), ivy::pong);
            assert(result);
            survivor = std::move(*result);
            assert(!result->is_bound());
        }
        assert(weak.expired() && !survivor.is_bound() && survivor.unbind());
    }
}

void timer_subscriptions() {
    using namespace std::chrono_literals;
    auto bus = require_bus(ivy::Bus::create("timers"));
    auto* ctx = bus.native_handle();
    int calls = 0;
    auto callback = [counter = std::make_unique<int>(0), &calls](std::chrono::milliseconds late) {
        assert(late == 3ms);
        calls = ++*counter;
    };
    auto timer = bus.bind_event(std::move(callback), ivy::every(1s));
    assert(timer && timer->is_bound());
    auto* original = ctx->timers.back();
    assert(original->period == 1000);
    timer_event(original, 3);
    assert(calls == 1);
    expect_error(IVY_EINVAL, timer->set_period(0ms));
    expect_error(IVY_EINVAL, timer->set_period(-1ms));
    expect_error(IVY_EINVAL, bus.bind_event([](auto) {}, ivy::every(0ms)));
    expect_error(IVY_EINVAL, bus.bind_event(ivy::Bus::TimerCallback{}, ivy::every(1s)));
    timer_error = IVY_ENOMEM;
    expect_error(IVY_ENOMEM, timer->set_period(20ms));
    expect_error(IVY_ENOMEM, bus.bind_event([](auto) {}, ivy::every(20ms)));
    timer_error = IVY_OK;
    timer_event(original, 3);
    assert(calls == 2 && !original->removed);

    // A timer may expire on the loop before the creating thread resumes.
    during_timer_create = [&] { timer_event(ctx->timers.back(), 3); };
    assert(timer->set_period(20ms));
    during_timer_create = {};
    auto* replacement = ctx->timers.back();
    assert(replacement != original && replacement->period == 20 && calls == 2);
    timer_event(original, 3);
    assert(original->removed && calls == 2);
    timer_event(replacement, 3);
    assert(calls == 3);

    auto other = bus.bind_event([](auto) {}, ivy::every(40ms));
    assert(other && timer->is_bound());
    auto moved = std::move(bus);
    assert(timer->is_bound() && other->is_bound());
    expect_error(IVY_ESTATE, bus.bind_event([](auto) {}, ivy::every(1s)));
    during_timer_create = [&] { assert(timer->unbind()); };
    expect_error(IVY_ESTATE, timer->set_period(30ms));
    during_timer_create = {};
    assert(!timer->is_bound() && other->is_bound());
    timer_event(replacement, 3);
    timer_event(ctx->timers.back(), 3);
    assert(replacement->removed && ctx->timers.back()->removed && calls == 3);
    expect_error(IVY_ESTATE, timer->set_period(1s));
    assert(moved.stop());
    assert(!other->is_bound());
    expect_error(IVY_ESTOPPED, other->set_period(1s));
    expect_error(IVY_ESTOPPED, moved.bind_event([](auto) {}, ivy::every(1s)));
    assert(other->unbind());
}

void timer_callback_lifetimes() {
    using namespace std::chrono_literals;
    ivy::TimerSubscription survivor;
    std::weak_ptr<int> weak;
    {
        auto bus = require_bus(ivy::Bus::create("timer lifetime"));
        auto* ctx = bus.native_handle();
        auto capture = std::make_shared<int>(42);
        weak = capture;
        std::latch entered(1), release(1);
        auto timer = bus.bind_event([value = std::move(capture), &entered, &release](auto) {
            entered.count_down();
            release.wait();
            assert(*value == 42);
        }, ivy::every(1s));
        assert(timer);
        auto* native = ctx->timers.back();
        std::thread thread([&] { timer_event(native); });
        entered.wait();
        assert(timer->unbind() && !timer->is_bound() && !weak.expired());
        release.count_down();
        thread.join();
        assert(weak.expired() && native->removed);

        std::optional<ivy::TimerSubscription> self;
        auto self_result = bus.bind_event([&](auto) { self.reset(); }, ivy::every(1s));
        assert(self_result);
        self.emplace(std::move(*self_result));
        native = ctx->timers.back();
        timer_event(native);
        assert(!self && native->removed);

        std::optional<ivy::TimerSubscription> changing;
        auto changed = bus.bind_event([&](auto) { assert(changing->set_period(20ms)); }, ivy::every(1s));
        assert(changed);
        changing.emplace(std::move(*changed));
        native = ctx->timers.back();
        timer_event(native);
        assert(native->removed && ctx->timers.back()->period == 20 && changing->is_bound());
        assert(changing->unbind());

        auto failing = bus.bind_event([](auto) { throw 42; }, ivy::every(1s));
        assert(failing);
        timer_event(ctx->timers.back());
        expect_error(ivy::Error::callback_failed, bus.take_callback_error());
        assert(bus.state() == IVY_CTX_STOPPED);
    }
    {
        auto bus = require_bus(ivy::Bus::create("surviving timer"));
        auto capture = std::make_shared<int>(42);
        weak = capture;
        auto timer = bus.bind_event([value = std::move(capture)](auto) {}, ivy::every(1s));
        assert(timer);
        survivor = std::move(*timer);
        assert(!timer->is_bound());
    }
    assert(weak.expired() && !survivor.is_bound() && survivor.unbind());
    expect_error(IVY_ESTATE, survivor.set_period(1s));
}

void bus_filters() {
    auto a = require_bus(ivy::Bus::create("filter-a"));
    auto b = require_bus(ivy::Bus::create("filter-b"));
    std::string word = "TRACK-suffix";
    assert(a.set_filters({std::string_view(word).substr(0, 5), "STATUS"}));
    word.clear();
    assert((a.native_handle()->filters == std::vector<std::string>{"TRACK", "STATUS"}));
    const std::string_view other[] = {"OTHER"};
    assert(b.set_filters(other));
    assert(a.add_filter(std::string_view("EXTRA-suffix").substr(0, 5)));
    assert(a.remove_filter(std::string_view("TRACK-suffix").substr(0, 5)));
    assert((a.native_handle()->filters == std::vector<std::string>{"STATUS", "EXTRA"}));
    assert((b.native_handle()->filters == std::vector<std::string>{"OTHER"}));
    const std::string_view nul("BAD\0WORD", 8);
    expect_error(IVY_EINVAL, a.set_filters({"GOOD", nul}));
    expect_error(IVY_EINVAL, a.add_filter(nul));
    expect_error(IVY_EINVAL, a.remove_filter(nul));
    assert((a.native_handle()->filters == std::vector<std::string>{"STATUS", "EXTRA"}));
    assert(a.set_filters("ONE", std::string("TWO"), std::string_view("THREE")));
    assert((a.native_handle()->filters == std::vector<std::string>{"ONE", "TWO", "THREE"}));
    std::vector<std::string> collection{"FOUR", "FIVE"};
    assert(a.set_filters(collection));
    collection[0] = "MUTATED";
    assert((a.native_handle()->filters == std::vector<std::string>{"FOUR", "FIVE"}));
    assert(a.set_filters(std::vector<std::string>{"TEMPORARY"}));
    const char* literals[] = {"SIX", "SEVEN"};
    assert(a.set_filters(literals));
    assert((a.native_handle()->filters == std::vector<std::string>{"SIX", "SEVEN"}));
    assert(a.set_filters(collection | std::views::transform([](const auto& word) {
        return word + "-VIEW"; // Each element is a temporary string, owned before increment.
    })));
    assert((a.native_handle()->filters == std::vector<std::string>{"MUTATED-VIEW", "FIVE-VIEW"}));
    std::istringstream stream("EIGHT NINE");
    assert(a.set_filters(std::ranges::istream_view<std::string>(stream)));
    assert((a.native_handle()->filters == std::vector<std::string>{"EIGHT", "NINE"}));
    auto failing_range = std::views::iota(0, 2) | std::views::transform([](int i) -> std::string {
        if (i == 1) throw std::bad_alloc();
        return "PARTIAL";
    });
    expect_error(IVY_ENOMEM, a.set_filters(failing_range));
    assert((a.native_handle()->filters == std::vector<std::string>{"EIGHT", "NINE"}));
    auto invalid_range = std::views::iota(0, 2) | std::views::transform([](int i) -> std::string {
        if (i == 1) throw 42;
        return "PARTIAL";
    });
    expect_error(IVY_EINVAL, a.set_filters(invalid_range));
    assert((a.native_handle()->filters == std::vector<std::string>{"EIGHT", "NINE"}));
    assert(a.set_filters() && a.native_handle()->filters.empty());
    assert(a.set_filters("SINGLE") && a.native_handle()->filters == std::vector<std::string>{"SINGLE"});
    assert(a.set_filters(std::vector<std::string>{}) && a.native_handle()->filters.empty());
    assert(a.set_filters("STATUS", "EXTRA"));
    filter_error = IVY_ENOMEM;
    expect_error(IVY_ENOMEM, a.set_filters({"REPLACEMENT"}));
    expect_error(IVY_ENOMEM, a.add_filter("NEW"));
    expect_error(IVY_ENOMEM, a.remove_filter("STATUS"));
    filter_error = IVY_OK;
    assert((a.native_handle()->filters == std::vector<std::string>{"STATUS", "EXTRA"}));
    assert(a.set_filters({}) && a.native_handle()->filters.empty());
    assert(b.clear_filters() && b.native_handle()->filters.empty());
    auto moved = std::move(a);
    expect_error(IVY_ESTATE, a.set_filters({"A"}));
    expect_error(IVY_ESTATE, a.add_filter("A"));
    expect_error(IVY_ESTATE, a.remove_filter("A"));
    expect_error(IVY_ESTATE, a.clear_filters());
    assert(moved.stop());
    expect_error(IVY_ESTOPPED, moved.set_filters({"A"}));
    expect_error(IVY_ESTOPPED, moved.add_filter("A"));
    expect_error(IVY_ESTOPPED, moved.remove_filter("A"));
    expect_error(IVY_ESTOPPED, moved.clear_filters());
    static_assert(noexcept(b.set_filters({"A", "B"})));
    static_assert(noexcept(b.add_filter("A")));
    static_assert(noexcept(b.remove_filter("A")));
    static_assert(noexcept(b.clear_filters()));
}

void control_messages() {
    auto bus = require_bus(ivy::Bus::create("control"));
    _clnt_lst_dict peer{bus.native_handle()}, foreign{};
    expect_error(IVY_ESTATE, bus.send_die(&peer));
    expect_error(IVY_ESTATE, bus.send_error(&peer, 1, "error"));
    assert(bus.start());
    assert(bus.send_die(&peer) && control_kind == "die" && sent_peer == &peer);
    assert(bus.send_error(&peer, 7, "100% {} unchanged"));
    assert(control_kind == "error" && sent_id == 7 && sent_message == "100% {} unchanged");
    assert(bus.send_error(&peer, 8, "Error {}", 42) && sent_message == "Error 42");
    assert(bus.send_error(&peer, 9, std::string_view("slice-extra").substr(0, 5)));
    assert(sent_message == "slice");
    assert(bus.send_error(&peer, 0, std::string_view{}) && sent_message.empty());
    expect_error(IVY_EINVAL, bus.send_die(nullptr));
    expect_error(IVY_EINVAL, bus.send_die(&foreign));
    expect_error(IVY_EINVAL, bus.send_error(nullptr, 1, "text"));
    expect_error(IVY_EINVAL, bus.send_error(&foreign, 1, "text"));
    for (char invalid : {'\0', '\n', '\002', '\003'}) {
        std::string text = "bad";
        text += invalid;
        expect_error(IVY_EINVAL, bus.send_error(&peer, 1, text));
    }
    for (auto status : {IVY_EIO, IVY_ENOMEM, IVY_EFIFOFULL}) {
        send_error = status;
        expect_error(status, bus.send_die(&peer));
        expect_error(status, bus.send_error(&peer, 1, "text"));
    }
    send_error = IVY_OK;
    auto moved = std::move(bus);
    expect_error(IVY_ESTATE, bus.send_die(&peer));
    expect_error(IVY_ESTATE, bus.send_error(&peer, 1, "text"));
    assert(moved.stop());
    expect_error(IVY_ESTOPPED, moved.send_die(&peer));
    expect_error(IVY_ESTOPPED, moved.send_error(&peer, 1, "text"));
    static_assert(noexcept(moved.send_die(&peer)));
    static_assert(noexcept(moved.send_error(&peer, 1, std::string_view{})));
}

void limited_timers() {
    using namespace std::chrono_literals;
    auto bus = require_bus(ivy::Bus::create("limited timers"));
    auto* ctx = bus.native_handle();
    expect_error(IVY_EINVAL, bus.bind_event([](auto) {}, ivy::every(1ms, 0)));
    expect_error(IVY_EINVAL, bus.bind_event([](auto) {}, ivy::every(1ms, -1)));
    expect_error(IVY_EINVAL, bus.bind_event([](auto) {}, ivy::every(0ms, 1)));
    expect_error(IVY_EINVAL, bus.bind_event([](auto) {}, ivy::after(-1ms)));
    assert(ctx->timers.empty());

    int calls = 0;
    auto capture = std::make_shared<int>(42);
    std::weak_ptr<int> weak = capture;
    auto finite = bus.bind_event([value = std::move(capture), &calls](auto) {
        assert(*value == 42);
        ++calls;
    }, ivy::every(1s, 3));
    assert(finite && finite->is_bound());
    auto* original = ctx->timers.back();
    timer_event(original);
    assert(calls == 1 && finite->is_bound());
    timer_error = IVY_ENOMEM;
    expect_error(IVY_ENOMEM, finite->set_period(10ms));
    timer_error = IVY_OK;
    assert(finite->set_period(10ms));
    auto* changed = ctx->timers.back();
    timer_event(original);
    assert(original->removed && calls == 1);
    timer_event(changed);
    assert(calls == 2 && finite->is_bound());
    timer_event(changed);
    assert(calls == 3 && !finite->is_bound() && changed->removed && weak.expired());
    timer_event(changed);
    assert(calls == 3 && finite->unbind());
    expect_error(IVY_ESTATE, finite->set_period(1s));

    int once_calls = 0;
    during_timer_create = [&] { timer_event(ctx->timers.back()); };
    auto once = bus.bind_event([&](auto) { ++once_calls; }, ivy::after(0ms));
    during_timer_create = {};
    assert(once && once->is_bound() && once_calls == 0);
    auto* immediate = ctx->timers.back();
    assert(immediate->period == 0 && !immediate->removed);
    assert(once->set_period(0ms));
    auto* rescheduled = ctx->timers.back();
    timer_event(immediate);
    assert(immediate->removed && once_calls == 0);
    timer_event(rescheduled);
    assert(once_calls == 1 && !once->is_bound() && rescheduled->removed);
    expect_error(IVY_ESTATE, once->set_period(0ms));

    auto expiring = bus.bind_event([&](auto) { ++once_calls; }, ivy::every(1ms, 1));
    assert(expiring);
    auto* last = ctx->timers.back();
    during_timer_create = [&] { timer_event(last); };
    expect_error(IVY_ESTATE, expiring->set_period(10ms));
    during_timer_create = {};
    assert(once_calls == 2 && !expiring->is_bound());
    timer_event(ctx->timers.back());
    assert(ctx->timers.back()->removed && once_calls == 2);

    TimerId cancelled;
    {
        auto token = bus.bind_event([&](auto) { ++once_calls; }, ivy::after(1ms));
        assert(token);
        cancelled = ctx->timers.back();
    }
    timer_event(cancelled);
    assert(cancelled->removed && once_calls == 2);

    std::latch entered(1), release(1);
    capture = std::make_shared<int>(43);
    weak = capture;
    auto in_progress = bus.bind_event([value = std::move(capture), &entered, &release](auto) {
        entered.count_down();
        release.wait();
        assert(*value == 43);
    }, ivy::after(1ms));
    assert(in_progress);
    auto* running = ctx->timers.back();
    std::thread thread([&] { timer_event(running); });
    entered.wait();
    assert(!in_progress->is_bound() && !weak.expired());
    expect_error(IVY_ESTATE, in_progress->set_period(1ms));
    assert(in_progress->unbind());
    release.count_down();
    thread.join();
    assert(running->removed && weak.expired());
}

int main() {
    {
        auto bus = require_bus(ivy::Bus::create("run-result"));
        assert(!bus.run());
        assert(bus.start());
        last_error = IVY_ENOMEM;
        assert(bus.run()); // No stale TLS error may leak into the result.
        run_error = IVY_EIO;
        last_error = IVY_OK;
        assert(bus.run().error() == ivy::make_error_code(IVY_EIO));
        run_error = IVY_OK;
        auto moved = std::move(bus);
        assert(bus.run().error() == ivy::make_error_code(IVY_ESTATE));
    }

    static_assert(HasChange<ivy::Subscription>);
    static_assert(!HasChange<ivy::DirectSubscription>);
    static_assert(!std::is_convertible_v<ivy::DirectSubscription, ivy::Subscription>);
    static_assert(!std::is_copy_constructible_v<ivy::DirectSubscription>);
    static_assert(!std::is_copy_assignable_v<ivy::DirectSubscription>);
    static_assert(std::is_nothrow_move_constructible_v<ivy::DirectSubscription>);
    static_assert(std::is_nothrow_move_assignable_v<ivy::DirectSubscription>);
    static_assert(std::is_same_v<decltype(std::declval<ivy::Bus&>().bind_raw(
        std::declval<ivy::Bus::MessageCallback>(), "^regexp")),
        std::expected<ivy::Subscription, std::error_code>>);
    static_assert(std::is_same_v<decltype(std::declval<ivy::Bus&>().bind_raw(
        std::declval<ivy::Bus::MessageCallback>(), "^{}", 42)),
        std::expected<ivy::Subscription, std::error_code>>);
    static_assert(std::is_same_v<decltype(std::declval<ivy::Bus&>().bind_direct(
        std::declval<ivy::Bus::DirectCallback>())),
        std::expected<ivy::DirectSubscription, std::error_code>>);
    static_assert(!std::is_copy_constructible_v<ivy::Subscription>);
    static_assert(!std::is_copy_assignable_v<ivy::Subscription>);
    static_assert(std::is_nothrow_move_constructible_v<ivy::Subscription>);
    static_assert(std::is_nothrow_move_assignable_v<ivy::Subscription>);
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
    static_assert(!HasChange<ivy::EventSubscription>);
    static_assert(!HasChange<ivy::TimerSubscription>);
    static_assert(!std::is_copy_constructible_v<ivy::EventSubscription>);
    static_assert(!std::is_copy_constructible_v<ivy::TimerSubscription>);
    static_assert(std::is_nothrow_move_constructible_v<ivy::EventSubscription>);
    static_assert(std::is_nothrow_move_constructible_v<ivy::TimerSubscription>);
    static_assert(noexcept(std::declval<ivy::Bus&>().bind_event([](auto...) {}, ivy::pong)));
    static_assert(noexcept(std::declval<ivy::Bus&>().bind_event([](auto...) {}, ivy::remote_bindings)));
    constexpr auto schedule = ivy::every(std::chrono::milliseconds(1000));
    static_assert(noexcept(std::declval<ivy::Bus&>().bind_event([](auto) {}, schedule)));
    control_messages();
    bus_filters();
    event_subscriptions_and_ping();
    event_callback_lifetimes();
    limited_timers();
    timer_subscriptions();
    timer_callback_lifetimes();
    nonthrowing_boundaries();
    strings_and_lifecycle();
    move_only_callbacks();
    errors_and_callback_exceptions();
    subscriptions_and_formats();
    raw_callbacks_without_sender();
    direct_subscriptions();
    subscription_lifetimes();
    concurrent_unbind();
    subscription_exceptions();
    converted_subscriptions();
    conversion_diagnostics();
    concurrent_conversion_diagnostics();
    change_preserves_subscription();
    change_and_unbind();
    direct_token_lifetimes();
    anchoring_boundary();
    sending_and_transport_callback();
    assert(live_contexts == 0);
    std::cout << "C++ bus boundary tests passed\n";
}
