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
    auto result = bus.bind(std::move(callback), ivy::runtime_regexp(std::string_view(pattern).substr(0, pattern.size() - 7)));
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
    auto formatted = bus.bind(ignore, R"(^TRACK {} ([0-9]{{2}}) 100%$)", 42);
    assert(formatted);
    assert(ctx->bindings.back()->regexp == R"(^TRACK 42 ([0-9]{2}) 100%$)");
    auto braces = bus.bind_unanchored(ignore, "{}");
    assert(braces && ctx->bindings.back()->regexp == "{}");
    const auto count = ctx->bindings.size();
    auto invalid_format = bus.bind(ignore, "^{:{}d}", 42, -1);
    assert(!invalid_format && invalid_format.error() == ivy::make_error_code(IVY_EINVAL));
    assert(!bus.bind(ignore, ivy::runtime_regexp(std::string_view("a\0b", 3))));
    assert(!bus.bind(ivy::Bus::MessageCallback{}, "^regexp"));
    assert(!bus.bind(ivy::Bus::DirectCallback{}));
    assert(ctx->bindings.size() == count);

    bind_error = IVY_ENOMEM;
    auto failed = bus.bind(ignore, "^failed");
    bind_error = IVY_OK;
    assert(!failed && failed.error() == ivy::make_error_code(IVY_ENOMEM));
    assert(ctx->bindings.size() == count);
    ivy::Bus moved(std::move(bus));
    assert(result->is_bound());
    auto from_moved = bus.bind(ignore, "^failed");
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
    auto after_stop = moved.bind(ignore, "^failed");
    assert(!after_stop && after_stop.error() == ivy::make_error_code(IVY_ESTOPPED));
}

void direct_subscriptions() {
    auto bus = require_bus(ivy::Bus::create("direct"));
    auto* ctx = bus.native_handle();
    _clnt_lst_dict peer;
    int old_calls = 0;
    int new_calls = 0;
    auto old = bus.bind([value = std::make_unique<int>(7), &old_calls, &peer]
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

    auto replacement = bus.bind([&](IvyClientPtr, int id, std::string_view text) {
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
    auto failed = bus.bind([](IvyClientPtr, int, std::string_view) {});
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
        auto result = bus.bind([capture = std::move(capture)](IvyClientPtr, auto) {}, "^first");
        assert(result);
        survivor = std::move(*result);
        const int before = unbind_count;
        {
            auto scoped = bus.bind([](IvyClientPtr, auto) {}, "^scoped");
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
    auto result = bus.bind([capture = std::move(capture), &self, &weak_capture](IvyClientPtr, auto) {
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
    auto result = bus.bind([capture = std::move(capture), &entered, &release](IvyClientPtr, auto) {
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
    auto subscription = messages.bind([](IvyClientPtr, auto) { throw 41; }, "^exception");
    assert(subscription);
    auto* binding = messages.native_handle()->bindings.back();
    binding->callback(nullptr, binding->data, 0, nullptr);
    assert(messages.state() == IVY_CTX_STOPPED);
    expect_error(ivy::Error::callback_failed, messages.take_callback_error());

    auto direct = require_bus(ivy::Bus::create("direct exception"));
    auto direct_subscription = direct.bind([](IvyClientPtr, int, std::string_view) { throw 42; });
    assert(direct_subscription);
    direct.native_handle()->direct(nullptr, direct.native_handle()->direct_data, 0, nullptr);
    assert(direct.state() == IVY_CTX_STOPPED);
    expect_error(ivy::Error::callback_failed, direct.take_callback_error());
}

void change_preserves_subscription() {
    ivy::Subscription empty;
    expect_error(IVY_ESTATE, empty.change("^pattern"));
    ivy::Subscription survivor;
    {
        auto bus = require_bus(ivy::Bus::create("change"));
        int calls = 0;
        auto result = bus.bind([count = std::make_unique<int>(0), &calls](IvyClientPtr, auto) {
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
    auto result = bus.bind([](IvyClientPtr, auto) {}, "^initial");
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

    auto concurrent = bus.bind([](IvyClientPtr, auto) {}, "^concurrent");
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
    auto self_result = bus.bind([](IvyClientPtr, auto) {}, "^self");
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
        auto a = first.bind([](IvyClientPtr, int, std::string_view) {});
        auto b = second.bind([capture = std::move(capture)](IvyClientPtr, int, std::string_view) {});
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
    auto result = bus.bind(callback, "^PREFIX {}", 42);
    assert(result && validated_regexp == "^PREFIX 42");
    assert(result->change("^CHANGED {}", 43));
    assert(validated_regexp == "^CHANGED 43");

    std::string dynamic = "^DYNAMIC [0-9]{2}";
    assert(bus.bind(callback, ivy::runtime_regexp(dynamic)));
    assert(validated_regexp == dynamic);
    auto missing_anchor = bus.bind(callback, ivy::runtime_regexp("DYNAMIC"));
    assert(!missing_anchor && missing_anchor.error() == ivy::make_error_code(IVY_EUNANCHORED));

    validation_error = IVY_ENOMEM;
    auto failed = bus.bind(callback, "^REJECTED");
    assert(!failed && failed.error() == ivy::make_error_code(IVY_ENOMEM));
    expect_error(IVY_ENOMEM, result->change("^REJECTED"));
    assert(bus.native_handle()->bindings.front()->regexp == "^CHANGED 43");
    validation_error = IVY_OK;

    const int before = validations;
    auto unanchored = bus.bind_unanchored(callback, "ANYWHERE {}", 44);
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
    auto subscription = bus.bind(callback, "^original");
    assert(subscription);
    _clnt_lst_dict peer;
    for (bool allocation : {false, true}) {
        FailingCallback failing(allocation);
        const auto expected = allocation ? ivy::make_error_code(IVY_ENOMEM)
            : ivy::make_error_code(ivy::Error::callback_failed);
        auto check = [&](auto result) { assert(!result && result.error() == expected); };
        check(ivy::Bus::create("bad application callback", std::nullopt, failing));
        check(ivy::Bus::create("bad die callback", std::nullopt, {}, failing));
        check(bus.bind(failing, "^regexp"));
        check(bus.bind(failing, ivy::runtime_regexp("^regexp")));
        check(bus.bind(failing, "^regexp {}", 42));
        check(bus.bind_unanchored(failing, "regexp"));
        check(bus.bind_unanchored(failing, "regexp {}", 42));
        check(bus.bind(failing));
        check(bus.set_transport_error_callback(failing));
    }
    for (int kind : {0, 1, 2, 3}) {
        FailingFormat failing{kind};
        const auto expected = kind == 0 ? ivy::make_error_code(IVY_ENOMEM)
            : kind == 3 ? ivy::make_error_code(ivy::Error::formatter_failed)
            : ivy::make_error_code(IVY_EINVAL);
        auto check = [&](auto result) { assert(!result && result.error() == expected); };
        check(bus.send("{}", failing));
        check(bus.send(&peer, 1, "{}", failing));
        const auto report = bus.send_report("{}", failing);
        assert(report.error == expected && !report.system_error);
        assert(report.matched == 0 && report.accepted == 0 && report.failed == 0);
        check(bus.bind(callback, "^{}", failing));
        check(bus.bind_unanchored(callback, "{}", failing));
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
    static_assert(noexcept(bus.bind(callback, "^regexp")));
    static_assert(noexcept(bus.bind(callback, "^{}", 42)));
    static_assert(noexcept(subscription->change("^{}", 42)));
}









int main() {
    static_assert(HasChange<ivy::Subscription>);
    static_assert(!HasChange<ivy::DirectSubscription>);
    static_assert(!std::is_convertible_v<ivy::DirectSubscription, ivy::Subscription>);
    static_assert(!std::is_copy_constructible_v<ivy::DirectSubscription>);
    static_assert(!std::is_copy_assignable_v<ivy::DirectSubscription>);
    static_assert(std::is_nothrow_move_constructible_v<ivy::DirectSubscription>);
    static_assert(std::is_nothrow_move_assignable_v<ivy::DirectSubscription>);
    static_assert(std::is_same_v<decltype(std::declval<ivy::Bus&>().bind(
        std::declval<ivy::Bus::MessageCallback>(), "^regexp")),
        std::expected<ivy::Subscription, std::error_code>>);
    static_assert(std::is_same_v<decltype(std::declval<ivy::Bus&>().bind(
        std::declval<ivy::Bus::MessageCallback>(), "^{}", 42)),
        std::expected<ivy::Subscription, std::error_code>>);
    static_assert(std::is_same_v<decltype(std::declval<ivy::Bus&>().bind(
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
    nonthrowing_boundaries();
    strings_and_lifecycle();
    move_only_callbacks();
    errors_and_callback_exceptions();
    subscriptions_and_formats();
    direct_subscriptions();
    subscription_lifetimes();
    concurrent_unbind();
    subscription_exceptions();
    change_preserves_subscription();
    change_and_unbind();
    direct_token_lifetimes();
    anchoring_boundary();
    sending_and_transport_callback();
    assert(live_contexts == 0);
    std::cout << "C++ bus boundary tests passed\n";
}
