/*
 *	Ivy, C interface
 *
 *	Copyright (C) 1997-2000
 *	Centre d'Études de la Navigation Aérienne
 *
 *	Main functions
 *
 *	Authors: François-Régis Colin <fcolin@cena.dgac.fr>
 *		 Stéphane Chatty <chatty@cena.dgac.fr>
 *
 *	$Id: ivy.h 3588 2013-06-19 12:39:15Z bustico $
 *
 *	Please refer to file version.h for the
 *	copyright notice regarding this software
 */

/**
 * @file ivy.h
 * @brief Public C API for the Ivy bus library.
 *
 * @details
 * Ivy is a lightweight publish/subscribe bus. Applications advertise their
 * presence on a supervision bus, exchange regular messages matched by regular
 * expressions, and may also send direct control messages to a known peer.
 *
 * The MT-safe API is the context API: create one ::IvyContext per Ivy bus,
 * start it with ::IvyContextStart(), run its loop with
 * ::IvyContextMainLoop(), and pass the context explicitly to every operation.
 * This is the API new applications should use. The legacy functions without an
 * ::IvyContext argument are kept so older applications continue to compile;
 * they operate on a process default context and are documented in the
 * compatibility group.
 *
 * Typical MT-safe application:
 *
 * @code{.c}
 * #include "ivy.h"
 * #include <pthread.h>
 * #include <stdio.h>
 *
 * static void *loop_thread(void *data)
 * {
 *     IvyContextMainLoop((IvyContext *)data);
 *     return NULL;
 * }
 *
 * static void on_msg(IvyClientPtr app, void *user_data,
 *                    int argc, char **argv)
 * {
 *     IvyContext *ctx = (IvyContext *)user_data;
 *     const char *name = IvyContextGetApplicationName(ctx, app);
 *
 *     printf("%s says %s\n", name ? name : "peer",
 *            argc > 0 ? argv[0] : "");
 * }
 *
 * int main(void)
 * {
 *     IvyContext *ctx = IvyContextCreate("example", "example ready",
 *                                        NULL, NULL, NULL, NULL);
 *     pthread_t thread;
 *
 *     IvyContextBindMsg(ctx, on_msg, ctx, "^HELLO (.*)");
 *     IvyContextStart(ctx, "127:2010");
 *     pthread_create(&thread, NULL, loop_thread, ctx);
 *
 *     IvyContextSendMsg(ctx, "HELLO world");
 *     IvyContextStop(ctx);
 *     pthread_join(thread, NULL);
 *     IvyContextDestroy(ctx);
 *     return 0;
 * }
 * @endcode
 *
 * Return conventions:
 * - functions returning int usually return ::IVY_OK on success and a negative
 *   ::IvyStatus on hard failure;
 * - ::IvyContextSendMsg() and ::IvySendMsg() return the number of frames accepted
 *   locally, or a negative ::IvyStatus if any matching send failed;
 * - query functions returning pointers return NULL on failure and expose the
 *   detailed error through ::IvyGetLastError();
 * - buffer query functions return the required buffer size including the
 *   trailing NUL byte. If the supplied buffer is too small, the result is
 *   truncated and ::IvyGetLastError() is ::IVY_ENOMEM.
 */

#ifndef IVY_H
#define IVY_H

#include <stddef.h>

#include "timer.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef __GNUC__
#  define  __attribute__(x)  /*NOTHING*/
#endif

/**
 * @defgroup ivy_types Public Types And Status Codes
 * @brief Opaque handles, status codes, events, and callback signatures.
 * @{
 */

/**
 * @brief Mutable internal client handle.
 *
 * @details
 * This type is public for historical compatibility. New application code
 * should use ::IvyClientPtr, which is read-only from the caller's point of
 * view. Do not dereference either handle; their representation is private.
 */
typedef struct _clnt_lst_dict *RWIvyClientPtr;

/**
 * @brief Handle identifying a peer application known by a context.
 *
 * @details
 * A value is valid only while the peer is still connected to the same
 * ::IvyContext that produced it. Handles are typically received in callbacks
 * or returned by ::IvyContextGetApplication(). Do not store them past the
 * lifetime of the connection unless your application coordinates with
 * disconnection callbacks and context shutdown.
 *
 * @code{.c}
 * static void on_app(IvyClientPtr app, void *user_data,
 *                    IvyApplicationEvent event)
 * {
 *     IvyContext *ctx = (IvyContext *)user_data;
 *     if (event == IvyApplicationConnected) {
 *         printf("connected: %s\n",
 *                IvyContextGetApplicationName(ctx, app));
 *     }
 * }
 * @endcode
 */
typedef const struct _clnt_lst_dict *IvyClientPtr;

/**
 * @brief Opaque state for one Ivy bus instance.
 *
 * @details
 * One context owns one bus connection, one event loop state, one socket set,
 * one timer state, and its bindings. Multiple contexts can coexist in the same
 * process and may be driven by distinct threads.
 */
typedef struct IvyContext IvyContext;

/**
 * @brief Status values returned by Ivy APIs.
 */
typedef enum {
	IVY_OK = 0,       /**< Success. */
	IVY_ESTOPPED = -1, /**< Operation rejected because the context is stopping or stopped. */
	IVY_ESTATE = -2, /**< Operation is invalid in the current context state. */
	IVY_EINVAL = -3, /**< Invalid argument or invalid handle for this context. */
	IVY_ENOMEM = -4, /**< Allocation failed or caller buffer was too small. */
	IVY_EIO = -5,    /**< Socket or transport I/O failure. */
	IVY_EUNANCHORED = -6, /**< Regexp does not satisfy the required start anchoring. */
	IVY_EFIFOFULL = -7 /**< A complete outgoing frame could not fit in the FIFO. */
} IvyStatus;

/** Per-subscription fan-out results, not remote delivery acknowledgements.
 * A peer with several matching subscriptions is counted once per subscription.
 */
typedef struct {
	size_t matched;
	size_t accepted; /**< Complete frames written or queued locally. */
	size_t failed;
	int system_error; /**< errno/WSA error for the first failure, or zero. */
} IvySendReport;

/** Transport failure observed by the loop, before the peer is disconnected.
 * The peer may be NULL during connection setup. The notification describes
 * the connection, including its queued frames, rather than a delivery receipt.
 * status is an Ivy error; system_error is errno/WSAGetLastError(), or zero
 * when the failure has no operating-system error. A recoverable FIFO or
 * allocation rejection is returned by the send call without this notification.
 */
typedef void (*IvyTransportErrorCallback)(IvyClientPtr app, void *user_data,
	IvyStatus status, int system_error);

/**
 * @brief Lifecycle state of an ::IvyContext.
 */
typedef enum {
	IVY_CTX_CREATED,   /**< Context exists but has not been started. */
	IVY_CTX_STARTING,  /**< Start is in progress. */
	IVY_CTX_RUNNING,   /**< Context is started and can exchange messages. */
	IVY_CTX_STOPPING,  /**< Stop has been requested. */
	IVY_CTX_STOPPED,   /**< Event loop and transport are stopped. */
	IVY_CTX_DESTROYED  /**< Context memory has been released. */
} IvyContextState;

/**
 * @brief Application-level connection event.
 */
typedef enum {
	IvyApplicationConnected,     /**< A peer has connected. */
	IvyApplicationDisconnected,  /**< A peer has disconnected. */
	IvyApplicationCongestion,    /**< A peer socket became congested. */
	IvyApplicationDecongestion,  /**< A peer socket left congestion. */
	IvyApplicationFifoFull       /**< A peer output FIFO is full. */
} IvyApplicationEvent;

/**
 * @brief Remote binding event reported by a peer.
 */
typedef enum {
	IvyAddBind,     /**< A peer added a regexp subscription. */
	IvyRemoveBind,  /**< A peer removed a regexp subscription. */
	IvyFilterBind,  /**< A peer regexp was filtered. */
	IvyChangeBind   /**< A peer changed a regexp subscription. */
} IvyBindEvent;

/**
 * @brief Callback called when a peer connects, disconnects, or changes send state.
 *
 * @param app Peer handle. The handle belongs to the context that invoked the callback.
 * @param user_data User pointer supplied to ::IvyContextCreate().
 * @param event Event kind.
 *
 * @code{.c}
 * static void on_application(IvyClientPtr app, void *user_data,
 *                            IvyApplicationEvent event)
 * {
 *     IvyContext *ctx = (IvyContext *)user_data;
 *
 *     if (event == IvyApplicationConnected)
 *         printf("%s joined\n", IvyContextGetApplicationName(ctx, app));
 * }
 * @endcode
 */
typedef void (*IvyApplicationCallback)(
	IvyClientPtr app, void *user_data, IvyApplicationEvent event);

/**
 * @brief Callback called when a peer advertises, removes, filters, or changes a regexp.
 *
 * @param app Peer handle.
 * @param user_data User pointer supplied to ::IvyContextSetBindCallback().
 * @param id Peer-side regexp identifier.
 * @param regexp Peer regexp text.
 * @param event Binding event kind.
 *
 * @code{.c}
 * static void on_bind(IvyClientPtr app, void *data, int id,
 *                     const char *regexp, IvyBindEvent event)
 * {
 *     IvyContext *ctx = (IvyContext *)data;
 *
 *     if (event == IvyAddBind)
 *         printf("%s listens to %s\n",
 *                IvyContextGetApplicationName(ctx, app), regexp);
 * }
 * @endcode
 */
typedef void (*IvyBindCallback)(
	IvyClientPtr app, void *user_data, int id, const char *regexp,
	IvyBindEvent event);

/**
 * @brief Callback called when a peer asks this application to terminate.
 *
 * @param app Peer that sent the die request.
 * @param user_data User pointer supplied to ::IvyContextCreate().
 * @param id Protocol identifier associated with the request.
 *
 * @code{.c}
 * static void on_die(IvyClientPtr app, void *data, int id)
 * {
 *     IvyContext *ctx = (IvyContext *)data;
 *     (void)app;
 *     (void)id;
 *     IvyContextStop(ctx);
 * }
 * @endcode
 */
typedef void (*IvyDieCallback)(IvyClientPtr app, void *user_data, int id);

/**
 * @brief Callback called when a ping response is received or times out.
 *
 * @param app Peer that answered, or peer whose ping timed out.
 * @param user_data User pointer supplied to ::IvyContextSetPongCallback()
 * or ::IvySetPongCallback().
 * @param round_trip_delay Positive round-trip time in microseconds, or a
 * negative timeout duration.
 *
 * @code{.c}
 * static void on_pong(IvyClientPtr app, void *user_data, int delay)
 * {
 *     IvyContext *ctx = (IvyContext *)user_data;
 *     const char *name = IvyContextGetApplicationName(ctx, app);
 *
 *     if (delay >= 0)
 *         printf("%s: pong in %.3f ms\n", name ? name : "peer", delay / 1000.0);
 * }
 * @endcode
 */
typedef void (*IvyPongCallback)(
	IvyClientPtr app, void *user_data, int round_trip_delay);

/**
 * @brief Callback called when an incoming Ivy message matches a local regexp.
 *
 * @param app Peer that sent the message.
 * @param user_data User pointer supplied to ::IvyContextBindMsg().
 * @param argc Number of captured regexp groups.
 * @param argv Captured regexp group values. The array is valid only during the callback.
 *
 * @code{.c}
 * static void on_hello(IvyClientPtr app, void *data, int argc, char **argv)
 * {
 *     IvyContext *ctx = (IvyContext *)data;
 *
 *     printf("%s says %s\n", IvyContextGetApplicationName(ctx, app),
 *            argc > 0 ? argv[0] : "");
 * }
 * @endcode
 */
typedef void (*MsgCallback)(
	IvyClientPtr app, void *user_data, int argc, char **argv);

/**
 * @brief Callback called when a direct message is received.
 *
 * @param app Peer that sent the direct message.
 * @param user_data User pointer supplied to ::IvyContextBindDirectMsg().
 * @param id Application-defined direct message identifier.
 * @param msg Direct message payload. The string is valid only during the callback.
 *
 * @code{.c}
 * static void on_direct(IvyClientPtr app, void *data, int id, char *msg)
 * {
 *     IvyContext *ctx = (IvyContext *)data;
 *     printf("direct %d from %s: %s\n", id,
 *            IvyContextGetApplicationName(ctx, app), msg);
 * }
 * @endcode
 */
typedef void (*MsgDirectCallback)(
	IvyClientPtr app, void *user_data, int id, char *msg);

/**
 * @brief Handle identifying one local regexp binding.
 *
 * @details
 * The handle is returned by ::IvyContextBindMsg() and is later passed to
 * ::IvyContextChangeMsg() or ::IvyContextUnbindMsg(). It belongs to the
 * context that created it.
 *
 * @code{.c}
 * MsgRcvPtr bind = IvyContextBindMsg(ctx, on_hello, ctx, "^HELLO (.*)");
 * IvyContextChangeMsg(ctx, bind, "^HI (.*)");
 * IvyContextUnbindMsg(ctx, bind);
 * @endcode
 */
typedef struct _msg_rcv *MsgRcvPtr;

/** @} */

/**
 * @defgroup ivy_context_api Contextual MT-safe API
 * @brief Preferred API for new applications.
 * @{
 */

/**
 * @brief Create a new Ivy context.
 *
 * @param AppName Application name advertised on the bus. If NULL, Ivy uses its default name.
 * @param ready Optional ready message sent after start. May be NULL.
 * @param callback Optional application event callback.
 * @param data User pointer passed to @p callback.
 * @param die_callback Optional die callback.
 * @param die_data User pointer passed to @p die_callback.
 * @return A new context, or NULL on allocation failure.
 *
 * @details
 * The context starts in ::IVY_CTX_CREATED state. It owns its buffers, sockets,
 * bindings and event-loop state. Destroy it with ::IvyContextDestroy().
 *
 * @code{.c}
 * IvyContext *ctx = IvyContextCreate("radar", "radar ready",
 *                                    on_application, NULL,
 *                                    on_die, NULL);
 * if (!ctx)
 *     fprintf(stderr, "create failed: %d\n", IvyGetLastError());
 * @endcode
 */
IvyContext *IvyContextCreate(
	 const char *AppName,
	 const char *ready,
	 IvyApplicationCallback callback,
	 void *data,
	 IvyDieCallback die_callback,
	 void *die_data
	 );

/**
 * @brief Start one context on an Ivy bus.
 *
 * @param ctx Context created by ::IvyContextCreate().
 * @param bus Bus address, for example "127:2010" or "192.168.1:2010".
 * If NULL or empty, Ivy uses the IVYBUS environment variable, then the compiled default.
 * @return ::IVY_OK on success, or a negative ::IvyStatus.
 *
 * @details
 * A context can currently be started only once from ::IVY_CTX_CREATED. After a
 * successful start, drive it with ::IvyContextMainLoop() or repeated
 * ::IvyContextIdle() calls.
 *
 * @code{.c}
 * if (IvyContextStart(ctx, "127:2010") != IVY_OK)
 *     fprintf(stderr, "start failed: %d\n", IvyGetLastError());
 * @endcode
 */
int IvyContextStart(IvyContext *ctx, const char *bus);

/**
 * @brief Stop a running context.
 *
 * @param ctx Context to stop.
 * @return ::IVY_OK on success, or a negative ::IvyStatus.
 *
 * @details
 * Stop is idempotent. On POSIX, calling this from another thread wakes the
 * context loop. The function waits until the loop has observed the stop
 * request, except when called from the owner loop itself.
 *
 * @code{.c}
 * IvyContextStop(ctx);
 * pthread_join(loop_thread, NULL);
 * @endcode
 */
int IvyContextStop(IvyContext *ctx);

/**
 * @brief Destroy a context and release its resources.
 *
 * @param ctx Context to destroy.
 * @return ::IVY_OK on success, or a negative ::IvyStatus.
 *
 * @details
 * If the context is still running, destruction first requests a stop. Do not
 * use any ::IvyClientPtr or ::MsgRcvPtr obtained from this context after it is
 * destroyed.
 *
 * @code{.c}
 * IvyContextStop(ctx);
 * IvyContextDestroy(ctx);
 * ctx = NULL;
 * @endcode
 */
int IvyContextDestroy(IvyContext *ctx);

/**
 * @brief Run the context event loop until the context stops.
 *
 * @param ctx Context whose event loop should run.
 *
 * @details
 * This call blocks. In MT-safe applications, it is common to run one loop
 * thread per context. Call ::IvyContextStop() from another thread or from a
 * callback to leave the loop.
 *
 * @code{.c}
 * static void *loop_thread(void *arg)
 * {
 *     IvyContextMainLoop((IvyContext *)arg);
 *     return NULL;
 * }
 * @endcode
 */
void IvyContextMainLoop(IvyContext *ctx);

/**
 * @brief Process pending events once without entering a permanent loop.
 *
 * @param ctx Context to service.
 *
 * @details
 * Use this when an application owns the outer loop and wants to periodically
 * let Ivy process pending work.
 *
 * @code{.c}
 * while (running) {
 *     app_poll_once();
 *     IvyContextIdle(ctx);
 * }
 * @endcode
 */
void IvyContextIdle(IvyContext *ctx);

/**
 * @brief Install or replace the callback for remote binding events.
 *
 * @param ctx Context to configure.
 * @param bind_callback Callback, or NULL to disable binding notifications.
 * @param bind_data User pointer passed to @p bind_callback.
 * @return ::IVY_OK on success, or a negative ::IvyStatus.
 *
 * @code{.c}
 * IvyContextSetBindCallback(ctx, on_bind, ctx);
 * @endcode
 */
int IvyContextSetBindCallback(IvyContext *ctx,
			  IvyBindCallback bind_callback,
			  void *bind_data );

/**
 * @brief Install or replace the callback for ping responses.
 *
 * @param ctx Context to configure.
 * @param pong_callback Callback, or NULL to disable ping handling.
 * @param pong_data User pointer passed to @p pong_callback. May be NULL.
 * @return ::IVY_OK on success, or a negative ::IvyStatus.
 *
 * @details
 * ::IvyContextSendPing() returns ::IVY_ESTATE if no pong callback is installed.
 * Ivy does not own @p pong_data. Keep it valid while callbacks may still use
 * it; replacing or disabling the callback does not wait for an in-flight call.
 *
 * @code{.c}
 * IvyContextSetPongCallback(ctx, on_pong, ctx);
 * IvyContextSendPing(ctx, app);
 * @endcode
 */
int IvyContextSetPongCallback(IvyContext *ctx,
			  IvyPongCallback pong_callback,
			  void *pong_data );

/** Install an optional transport-error callback. NULL disables it.
 * Called by the event loop outside internal locks. No notification is promised
 * after the loop stops; immediate send failures are always returned to callers.
 * Replacing the callback does not wait for an invocation already in progress;
 * keep its user_data valid until that invocation returns.
 * Returns IVY_OK, IVY_EINVAL for a NULL context, or IVY_ESTOPPED after stop.
 */
int IvyContextSetTransportErrorCallback(IvyContext *ctx,
	IvyTransportErrorCallback callback, void *user_data);
int IvySetTransportErrorCallback(IvyTransportErrorCallback callback, void *user_data);

/**
 * @brief Install or replace the callback for direct messages.
 *
 * @param ctx Context to configure.
 * @param callback Callback, or NULL to disable direct messages.
 * @param user_data User pointer passed to @p callback.
 * @return ::IVY_OK on success, or a negative ::IvyStatus.
 *
 * @code{.c}
 * IvyContextBindDirectMsg(ctx, on_direct, ctx);
 * @endcode
 */
int IvyContextBindDirectMsg(IvyContext *ctx,
			  MsgDirectCallback callback, void *user_data);

/**
 * @brief Subscribe to messages matching a regular expression.
 *
 * @param ctx Context that owns the binding.
 * @param callback Callback called when a message matches.
 * @param user_data User pointer passed to @p callback.
 * @param fmt_regexp printf-style format string producing the regexp.
 * @param ... Arguments for @p fmt_regexp.
 * @return A binding handle, or NULL on failure.
 *
 * @details
 * Capturing groups in the regexp are passed to the callback as argv entries.
 * Keep the returned ::MsgRcvPtr if you need to change or remove the binding.
 *
 * @code{.c}
 * MsgRcvPtr bind = IvyContextBindMsg(ctx, on_msg, ctx,
 *                                    "^TRACK ([0-9]+) (.*)");
 * if (!bind)
 *     fprintf(stderr, "bind failed: %d\n", IvyGetLastError());
 * @endcode
 */
MsgRcvPtr IvyContextBindMsg(IvyContext *ctx,
	 MsgCallback callback, void *user_data, const char *fmt_regexp, ... )
__attribute__((format(printf,4,5))) ;

/**
 * @brief Change an existing local message subscription.
 *
 * @param ctx Context that owns @p msg.
 * @param msg Binding returned by ::IvyContextBindMsg().
 * @param fmt_regex printf-style format string producing the new regexp.
 * @param ... Arguments for @p fmt_regex.
 * @return @p msg on success, or NULL on failure.
 *
 * @code{.c}
 * if (!IvyContextChangeMsg(ctx, bind, "^AIRCRAFT %s (.*)", callsign))
 *     fprintf(stderr, "change failed: %d\n", IvyGetLastError());
 * @endcode
 */
MsgRcvPtr IvyContextChangeMsg(IvyContext *ctx,
	 MsgRcvPtr msg, const char *fmt_regex, ... )
__attribute__((format(printf,3,4)));

/**
 * @brief Validate a start-anchored regexp without registering it.
 *
 * @param regexp Regexp text, not a printf format. It must begin with ^.
 * @return IVY_OK when PCRE2 recognizes the expanded regexp as anchored,
 * IVY_EUNANCHORED when the leading ^ is missing or the expanded regexp is not
 * recognized as anchored, IVY_EINVAL for other invalid arguments or regexp
 * syntax, IVY_ENOMEM on allocation failure, or IVY_ESTATE without PCRE2.
 *
 * Ivy interval syntax is expanded before validation. The original expression
 * is not rewritten, and PCRE2_ANCHORED is not forced on the compiled pattern.
 * This context-independent check does not register a subscription or run JIT.
 */
int IvyValidateAnchoredRegexp(const char *regexp);

/**
 * @brief Remove a local message subscription.
 *
 * @param ctx Context that owns @p msg.
 * @param msg Binding returned by ::IvyContextBindMsg().
 * @return ::IVY_OK on success, or a negative ::IvyStatus.
 *
 * @code{.c}
 * IvyContextUnbindMsg(ctx, bind);
 * bind = NULL;
 * @endcode
 */
int IvyContextUnbindMsg(IvyContext *ctx, MsgRcvPtr msg);

/**
 * @brief Send an Ivy protocol error to one peer.
 *
 * @param ctx Context that owns @p app.
 * @param app Peer handle.
 * @param id Application/protocol identifier associated with the error.
 * @param fmt printf-style format string for the error message.
 * @param ... Arguments for @p fmt.
 * @return ::IVY_OK on success, or a negative ::IvyStatus.
 *
 * @code{.c}
 * IvyContextSendError(ctx, app, 42, "bad command: %s", command);
 * @endcode
 */
int IvyContextSendError(IvyContext *ctx,
	 IvyClientPtr app, int id, const char *fmt, ... )
__attribute__((format(printf,4,5))) ;

/**
 * @brief Send a normal Ivy message to all matching peers on one context.
 *
 * @param ctx Context to send on.
 * @param fmt_message printf-style format string for the message.
 * @param ... Arguments for @p fmt_message.
 * @return Number of frames accepted locally, or a negative ::IvyStatus if any
 * matching send failed. Zero matches is success.
 *
 * @details
 * The message is matched against remote subscriptions known by @p ctx. In a
 * multi-bus program, call this once per context if the same message should be
 * sent on several buses. A peer with multiple matching subscriptions counts
 * multiple times. Success means a complete frame was written or queued, not
 * that the peer received it. Sending continues after individual failures;
 * use ::IvyContextSendMsgEx() when partial counts are needed. Retrying the
 * whole message can duplicate frames already accepted by other subscriptions.
 * IVY_EIO reports a socket failure, IVY_ENOMEM an allocation failure, and
 * IVY_EFIFOFULL a frame that cannot fit in the outgoing FIFO. Newlines, Ivy
 * argument separators (bytes 2 and 3), and formatted NUL bytes are rejected
 * with IVY_EINVAL before matching.
 *
 * @code{.c}
 * int recipients = IvyContextSendMsg(ctx, "TRACK %d %s", id, label);
 * if (recipients < 0)
 *     fprintf(stderr, "send failed: %d\n", recipients);
 * @endcode
 */
int IvyContextSendMsg(IvyContext *ctx, const char *fmt_message, ... )
__attribute__((format(printf,2,3)));

/** Send with a report, including on partial failure. Returns IVY_OK only if
 * every matching frame was accepted locally; otherwise the first error.
 * Sending continues to the remaining subscriptions after an individual failure.
 * matched == accepted + failed; errors before matching leave all counts zero.
 * report must not be NULL. A negative result does not undo accepted frames.
 */
int IvyContextSendMsgEx(IvyContext *ctx, IvySendReport *report, const char *fmt_message, ...)
__attribute__((format(printf,3,4)));
int IvySendMsgEx(IvySendReport *report, const char *fmt_message, ...)
__attribute__((format(printf,2,3)));

/**
 * @brief Send a direct message to one peer.
 *
 * @param ctx Context that owns @p app.
 * @param app Peer handle.
 * @param id Application-defined direct-message identifier.
 * @param msg NUL-terminated text, without newline or Ivy argument separators.
 * @return ::IVY_OK when the complete frame is written or queued locally, or a
 * negative ::IvyStatus (including IVY_EIO, IVY_ENOMEM and IVY_EFIFOFULL).
 * No delivery acknowledgement is implied; deferred failures are reported to
 * the optional ::IvyTransportErrorCallback.
 *
 * @code{.c}
 * IvyContextSendDirectMsg(ctx, app, 7, "reload");
 * @endcode
 */
int IvyContextSendDirectMsg(IvyContext *ctx, IvyClientPtr app, int id, char *msg);

/**
 * @brief Ask one peer to terminate.
 *
 * @param ctx Context that owns @p app.
 * @param app Peer handle.
 * @return ::IVY_OK on success, or a negative ::IvyStatus.
 *
 * @code{.c}
 * IvyClientPtr peer = IvyContextGetApplication(ctx, "old-agent");
 * if (peer)
 *     IvyContextSendDieMsg(ctx, peer);
 * @endcode
 */
int IvyContextSendDieMsg(IvyContext *ctx, IvyClientPtr app);

/**
 * @brief Send a ping to one peer.
 *
 * @param ctx Context that owns @p app.
 * @param app Peer handle.
 * @return ::IVY_OK on success, or a negative ::IvyStatus.
 *
 * @details
 * Install a pong callback with ::IvyContextSetPongCallback() before calling
 * this function.
 *
 * @code{.c}
 * IvyContextSetPongCallback(ctx, on_pong, ctx);
 * IvyContextSendPing(ctx, app);
 * @endcode
 */
int IvyContextSendPing(IvyContext *ctx, IvyClientPtr app);

/**
 * @brief Create a timer attached to a context event loop.
 *
 * @param ctx Context whose loop owns the timer.
 * @param count Number of expirations, or ::TIMER_LOOP for an infinite timer.
 * @param timeout Timer period in milliseconds.
 * @param cb Callback called from the context loop thread.
 * @param user_data User pointer passed to @p cb.
 * @return Timer handle, or NULL on failure.
 *
 * @details
 * This is the MT-safe entry point for timers used by Ivy tools and
 * applications. If the context loop is already active and the caller is not
 * the loop thread, timer creation is posted to the owner loop before the
 * function returns. ::TimerModify() and ::TimerRemove() can be used with the
 * returned handle.
 *
 * @code{.c}
 * static void on_timer(TimerId id, void *data, unsigned long delta)
 * {
 *     IvyContext *ctx = (IvyContext *)data;
 *     (void)id;
 *     (void)delta;
 *     IvyContextSendMsg(ctx, "TICK");
 * }
 *
 * TimerId timer = IvyContextTimerRepeatAfter(ctx, TIMER_LOOP, 1000,
 *                                            on_timer, ctx);
 * if (!timer)
 *     fprintf(stderr, "timer failed: %d\n", IvyGetLastError());
 * @endcode
 */
TimerId IvyContextTimerRepeatAfter(IvyContext *ctx, int count, long timeout,
	TimerCb cb, void *user_data);

/**
 * @brief Return the advertised name for a peer owned by a context.
 *
 * @param ctx Context that owns @p app.
 * @param app Peer handle.
 * @return Peer name, or NULL if @p app is invalid for @p ctx.
 *
 * @details
 * The returned pointer is owned by Ivy and remains valid only while the peer is
 * connected to @p ctx.
 *
 * @code{.c}
 * const char *name = IvyContextGetApplicationName(ctx, app);
 * printf("peer=%s\n", name ? name : "(unknown)");
 * @endcode
 */
const char *IvyContextGetApplicationName(IvyContext *ctx, IvyClientPtr app);

/**
 * @brief Return the remote host name for a peer owned by a context.
 *
 * @param ctx Context that owns @p app.
 * @param app Peer handle.
 * @return Host name, or NULL if @p app is invalid for @p ctx.
 *
 * @code{.c}
 * const char *host = IvyContextGetApplicationHost(ctx, app);
 * printf("host=%s\n", host ? host : "(unknown)");
 * @endcode
 */
const char *IvyContextGetApplicationHost(IvyContext *ctx, IvyClientPtr app);

/**
 * @brief Find a connected peer by application name.
 *
 * @param ctx Context to query.
 * @param name Application name to search.
 * @return Peer handle, or NULL when not found or on error.
 *
 * @code{.c}
 * IvyClientPtr app = IvyContextGetApplication(ctx, "logger");
 * if (app)
 *     IvyContextSendDirectMsg(ctx, app, 1, "flush");
 * @endcode
 */
IvyClientPtr IvyContextGetApplication(IvyContext *ctx, char *name);

/**
 * @brief Return a separator-joined list of connected application names.
 *
 * @param ctx Context to query.
 * @param sep Separator appended after each name.
 * @return Context-owned string, or NULL on error.
 *
 * @warning
 * This compatibility query returns a buffer owned by @p ctx. New MT-safe code
 * should use ::IvyContextGetApplicationListBuffer().
 *
 * @code{.c}
 * printf("apps: %s\n", IvyContextGetApplicationList(ctx, ","));
 * @endcode
 */
char *IvyContextGetApplicationList(IvyContext *ctx, const char *sep);

/**
 * @brief Copy the connected application list into a caller-provided buffer.
 *
 * @param ctx Context to query.
 * @param buffer Destination buffer, or NULL for a sizing query.
 * @param buffer_size Size of @p buffer in bytes. Use 0 for a sizing query.
 * @param sep Separator appended after each name.
 * @return Required buffer size in bytes, including the trailing NUL, or a negative ::IvyStatus.
 *
 * @details
 * If @p buffer_size is non-zero and the return value is greater than
 * @p buffer_size, the output was truncated and ::IvyGetLastError() is
 * ::IVY_ENOMEM. The returned value can be used directly as the allocation size.
 *
 * @code{.c}
 * int size = IvyContextGetApplicationListBuffer(ctx, NULL, 0, ",");
 * if (size > 0) {
 *     char *apps = malloc((size_t)size);
 *     IvyContextGetApplicationListBuffer(ctx, apps, (size_t)size, ",");
 *     puts(apps);
 *     free(apps);
 * }
 * @endcode
 */
int IvyContextGetApplicationListBuffer(IvyContext *ctx,
	char *buffer, size_t buffer_size, const char *sep);

/**
 * @brief Return the regexps currently advertised by a peer.
 *
 * @param ctx Context that owns @p app.
 * @param app Peer handle.
 * @return NULL-terminated context-owned array of regexp strings, or NULL on error.
 *
 * @warning
 * This compatibility query returns storage owned by @p ctx. New MT-safe code
 * should use ::IvyContextGetApplicationMessagesBuffer().
 *
 * @code{.c}
 * char **messages = IvyContextGetApplicationMessages(ctx, app);
 * for (char **it = messages; it && *it; ++it)
 *     puts(*it);
 * @endcode
 */
char **IvyContextGetApplicationMessages(IvyContext *ctx, IvyClientPtr app);

/**
 * @brief Copy the regexps advertised by a peer into a caller-provided buffer.
 *
 * @param ctx Context that owns @p app.
 * @param app Peer handle.
 * @param buffer Destination buffer, or NULL for a sizing query.
 * @param buffer_size Size of @p buffer in bytes. Use 0 for a sizing query.
 * @param sep Separator appended after each regexp.
 * @return Required buffer size in bytes, including the trailing NUL, or a negative ::IvyStatus.
 *
 * @code{.c}
 * int size = IvyContextGetApplicationMessagesBuffer(ctx, app, NULL, 0, "\n");
 * if (size > 0) {
 *     char *regexps = malloc((size_t)size);
 *     IvyContextGetApplicationMessagesBuffer(ctx, app, regexps,
 *                                            (size_t)size, "\n");
 *     fputs(regexps, stdout);
 *     free(regexps);
 * }
 * @endcode
 */
int IvyContextGetApplicationMessagesBuffer(IvyContext *ctx,
	IvyClientPtr app, char *buffer, size_t buffer_size, const char *sep);

/**
 * @brief Return the lifecycle state of a context.
 *
 * @param ctx Context to inspect.
 * @return Current state. If @p ctx is NULL, returns ::IVY_CTX_DESTROYED and sets ::IVY_EINVAL.
 *
 * @code{.c}
 * if (IvyContextGetState(ctx) == IVY_CTX_RUNNING)
 *     IvyContextSendMsg(ctx, "ALIVE");
 * @endcode
 */
IvyContextState IvyContextGetState(const IvyContext *ctx);

/**
 * @brief Return the last Ivy status for the calling thread.
 *
 * @return Last status set by an Ivy API on the current thread.
 *
 * @details
 * The error slot is thread-local. Use it after functions whose normal result
 * cannot directly encode all status details, such as pointer-returning queries
 * or buffer queries that return a positive required size while reporting
 * truncation.
 *
 * @code{.c}
 * char small[4];
 * int need = IvyContextGetApplicationListBuffer(ctx, small, sizeof(small), ",");
 * if (need > (int)sizeof(small) && IvyGetLastError() == IVY_ENOMEM)
 *     printf("need %d bytes\n", need);
 * @endcode
 */
IvyStatus IvyGetLastError(void);

/** @} */

/**
 * @defgroup ivy_filters Filtering API
 * @brief Per-context filtering of regexp subscriptions advertised by peers.
 *
 * Each context owns an initially empty list of message-class words. An empty
 * list accepts every regexp. With a nonempty list, Ivy checks the first literal
 * word after ^ against the declared classes. Prefixes are accepted: ^TRA.* may
 * match class TRACK. Expressions without a recognizable leading word are kept.
 * This is a send-side optimization, not a restriction on incoming messages.
 *
 * Filtering applies when a peer advertises or changes a regexp. Changes to the
 * filter list do not reprocess already accepted/rejected subscriptions. Rejected
 * advertisements generate IvyFilterBind outside internal locks. Configure filters
 * before start when the policy must apply to all initial advertisements.
 * Operations are serialized with subscription processing within the owning context.
 * @{
 */

/**
 * @brief Atomically replace the filter list of one context.
 * @param ctx Context to configure; no other context is affected.
 * @param argc Number of class words. Zero disables filtering for this context.
 * @param argv Class words, copied during the call; may be NULL when argc is zero.
 * Words must be nonempty and contain only ASCII letters, digits, underscore or hyphen.
 * @return IVY_OK, IVY_EINVAL for invalid arguments, IVY_ESTOPPED after stop,
 * or IVY_ENOMEM on allocation failure. Failure leaves the previous list intact.
 *
 * @code{.c}
 * const char *classes[] = { "STATUS", "TRACK" };
 * int status = IvyContextSetFilter(ctx, 2, classes);
 * if (status != IVY_OK)
 *     fprintf(stderr, "filter setup failed: %d\n", status);
 * @endcode
 */
int IvyContextSetFilter(IvyContext *ctx, int argc, const char **argv);

/**
 * @brief Add a class word to one context's filter list.
 * @param ctx Context to configure.
 * @param arg Nonempty class word, copied during the call; same syntax as IvyContextSetFilter().
 * @return IVY_OK, including if already present; IVY_EINVAL, IVY_ESTOPPED or IVY_ENOMEM
 * on failure. Failure leaves the previous list intact.
 */
int IvyContextAddFilter(IvyContext *ctx, const char *arg);

/**
 * @brief Remove a class word from one context's filter list.
 * @param ctx Context to configure.
 * @param arg Nonempty class word, with the same syntax as IvyContextSetFilter().
 * @return IVY_OK, including if absent, or IVY_EINVAL/IVY_ESTOPPED.
 * Removing the final word disables filtering for this context.
 */
int IvyContextRemoveFilter(IvyContext *ctx, const char *arg);

/**
 * @brief Replace filters for the calling thread's current/default context.
 * @param argc Number of class words; zero disables filtering.
 * @param argv Class words, copied during the call.
 * @return Same status as IvyContextSetFilter().
 * In 3.18 this really replaces the list; older implementations appended to it.
 * From a native Ivy callback, the current context is the callback's bus.
 * @see IvyContextSetFilter()
 */
int IvySetFilter(int argc, const char **argv);

/**
 * @brief Add one filter word to the current/default context.
 * @param arg Class word to copy.
 * @return Same status as IvyContextAddFilter().
 * @see IvyContextAddFilter()
 */
int IvyAddFilter(const char *arg);

/**
 * @brief Remove one filter word from the current/default context.
 * @param arg Class word to remove.
 * @return Same status as IvyContextRemoveFilter().
 * @see IvyContextRemoveFilter()
 */
int IvyRemoveFilter(const char *arg);

/** @} */

/**
 * @defgroup ivy_legacy_api Legacy Compatibility API
 * @brief Wrappers operating on the process default context.
 *
 * @details
 * These functions remain public so existing source code keeps compiling. New
 * projects should prefer the contextual API in @ref ivy_context_api, especially
 * in multi-thread or multi-bus programs.
 * @{
 */

/**
 * @brief Default application callback used by old examples.
 *
 * @param app Peer handle.
 * @param user_data Ignored.
 * @param event Application event.
 *
 * @deprecated New code should provide an application-specific callback that
 * receives the owning ::IvyContext through user data.
 *
 * @code{.c}
 * IvyContextCreate("legacy-print", NULL,
 *                  IvyDefaultApplicationCallback, NULL, NULL, NULL);
 * @endcode
 */
extern void IvyDefaultApplicationCallback(
	IvyClientPtr app, void *user_data, IvyApplicationEvent event);

/**
 * @brief Default bind callback used by old examples.
 *
 * @param app Peer handle.
 * @param user_data Ignored.
 * @param id Remote regexp identifier.
 * @param regexp Remote regexp text.
 * @param event Binding event.
 *
 * @deprecated New code should provide a callback that receives the owning
 * ::IvyContext through user data and uses contextual getters.
 *
 * @code{.c}
 * IvyContextSetBindCallback(ctx, IvyDefaultBindCallback, NULL);
 * @endcode
 */
extern void IvyDefaultBindCallback(
	IvyClientPtr app, void *user_data, int id, const char *regexp,
	IvyBindEvent event);

/**
 * @brief Initialize the process default context.
 *
 * @param AppName Application name.
 * @param ready Optional ready message.
 * @param callback Optional application callback.
 * @param data User pointer for @p callback.
 * @param die_callback Optional die callback.
 * @param die_data User pointer for @p die_callback.
 * @return ::IVY_OK on success, or a negative ::IvyStatus.
 *
 * @deprecated Use ::IvyContextCreate().
 *
 * @code{.c}
 * IvyInit("old-agent", "old-agent ready", NULL, NULL, NULL, NULL);
 * IvyStart("127:2010");
 * @endcode
 */
int IvyInit(
	 const char *AppName,
	 const char *ready,
	 IvyApplicationCallback callback,
	 void *data,
	 IvyDieCallback die_callback,
	 void *die_data
	 );

/**
 * @brief Destroy the process default context and terminate filter internals.
 *
 * @return ::IVY_OK on success, or a negative ::IvyStatus.
 *
 * @deprecated Use ::IvyContextDestroy() for explicit contexts.
 *
 * @code{.c}
 * IvyStop();
 * IvyTerminate();
 * @endcode
 */
int IvyTerminate(void);

/**
 * @brief Install the default-context bind callback.
 *
 * @param bind_callback Callback, or NULL to disable.
 * @param bind_data User pointer.
 * @return ::IVY_OK on success, or a negative ::IvyStatus.
 *
 * @deprecated Use ::IvyContextSetBindCallback().
 *
 * @code{.c}
 * IvySetBindCallback(on_bind, NULL);
 * @endcode
 */
int IvySetBindCallback(
			  IvyBindCallback bind_callback,
			  void *bind_data );

/**
 * @brief Install the default-context pong callback.
 *
 * @param pong_callback Callback, or NULL to disable.
 * @param pong_data User pointer passed to @p pong_callback. May be NULL.
 * @return ::IVY_OK on success, or a negative ::IvyStatus.
 *
 * @deprecated Use ::IvyContextSetPongCallback().
 *
 * @code{.c}
 * IvySetPongCallback(on_pong, ctx);
 * @endcode
 */
int IvySetPongCallback(
			  IvyPongCallback pong_callback,
			  void *pong_data );

/**
 * @brief Start the process default context.
 *
 * @param bus Bus address, IVYBUS fallback, or NULL for default.
 * @return ::IVY_OK on success, or a negative ::IvyStatus.
 *
 * @deprecated Use ::IvyContextStart().
 *
 * @code{.c}
 * IvyStart("127:2010");
 * @endcode
 */
int IvyStart (const char*);

/**
 * @brief Stop the process default context.
 *
 * @return ::IVY_OK on success, or a negative ::IvyStatus.
 *
 * @deprecated Use ::IvyContextStop().
 *
 * @code{.c}
 * IvyStop();
 * @endcode
 */
int IvyStop (void);

/**
 * @brief Return the peer name using the current/default context.
 *
 * @param app Peer handle.
 * @return Peer name, or "Unknown" if unavailable.
 *
 * @deprecated Use ::IvyContextGetApplicationName().
 *
 * @code{.c}
 * printf("%s\n", IvyGetApplicationName(app));
 * @endcode
 */
const char *IvyGetApplicationName( IvyClientPtr app );

/**
 * @brief Return the peer host using the current/default context.
 *
 * @param app Peer handle.
 * @return Host name, or NULL if unavailable.
 *
 * @deprecated Use ::IvyContextGetApplicationHost().
 *
 * @code{.c}
 * printf("%s\n", IvyGetApplicationHost(app));
 * @endcode
 */
const char *IvyGetApplicationHost( IvyClientPtr app );

/**
 * @brief Find a peer by name on the current/default context.
 *
 * @param name Application name.
 * @return Peer handle, or NULL if not found or on error.
 *
 * @deprecated Use ::IvyContextGetApplication().
 *
 * @code{.c}
 * IvyClientPtr app = IvyGetApplication("logger");
 * @endcode
 */
IvyClientPtr IvyGetApplication( char *name );

/**
 * @brief Return a default-context, context-owned application list string.
 *
 * @param sep Separator appended after each name.
 * @return Context-owned string, or NULL on error.
 *
 * @deprecated Use ::IvyContextGetApplicationListBuffer() when possible.
 *
 * @code{.c}
 * puts(IvyGetApplicationList(","));
 * @endcode
 */
char *IvyGetApplicationList(const char *sep);

/**
 * @brief Copy the default-context application list into a caller buffer.
 *
 * @param buffer Destination buffer, or NULL for sizing.
 * @param buffer_size Size of @p buffer in bytes.
 * @param sep Separator appended after each name.
 * @return Required buffer size including trailing NUL, or a negative ::IvyStatus.
 *
 * @deprecated Use ::IvyContextGetApplicationListBuffer() in new code.
 *
 * @code{.c}
 * int size = IvyGetApplicationListBuffer(NULL, 0, ",");
 * char *apps = malloc((size_t)size);
 * IvyGetApplicationListBuffer(apps, (size_t)size, ",");
 * free(apps);
 * @endcode
 */
int IvyGetApplicationListBuffer(char *buffer, size_t buffer_size, const char *sep);

/**
 * @brief Return regexps advertised by a peer on the current/default context.
 *
 * @param app Peer handle.
 * @return NULL-terminated context-owned array, or NULL on error.
 *
 * @deprecated Use ::IvyContextGetApplicationMessagesBuffer() when possible.
 *
 * @code{.c}
 * char **messages = IvyGetApplicationMessages(app);
 * for (char **it = messages; it && *it; ++it)
 *     puts(*it);
 * @endcode
 */
char **IvyGetApplicationMessages( IvyClientPtr app);

/**
 * @brief Copy peer regexps from the default context into a caller buffer.
 *
 * @param app Peer handle.
 * @param buffer Destination buffer, or NULL for sizing.
 * @param buffer_size Size of @p buffer in bytes.
 * @param sep Separator appended after each regexp.
 * @return Required buffer size including trailing NUL, or a negative ::IvyStatus.
 *
 * @deprecated Use ::IvyContextGetApplicationMessagesBuffer() in new code.
 *
 * @code{.c}
 * int size = IvyGetApplicationMessagesBuffer(app, NULL, 0, "\n");
 * char *regexps = malloc((size_t)size);
 * IvyGetApplicationMessagesBuffer(app, regexps, (size_t)size, "\n");
 * free(regexps);
 * @endcode
 */
int IvyGetApplicationMessagesBuffer(IvyClientPtr app,
	char *buffer, size_t buffer_size, const char *sep);

/**
 * @brief Subscribe to a message regexp on the current/default context.
 *
 * @param callback Callback called on matches.
 * @param user_data User pointer passed to @p callback.
 * @param fmt_regexp printf-style regexp format.
 * @param ... Arguments for @p fmt_regexp.
 * @return Binding handle, or NULL on failure.
 *
 * @deprecated Use ::IvyContextBindMsg().
 *
 * @code{.c}
 * MsgRcvPtr bind = IvyBindMsg(on_msg, NULL, "^HELLO (.*)");
 * @endcode
 */
MsgRcvPtr IvyBindMsg(
	MsgCallback callback, void *user_data, const char *fmt_regexp, ... )
__attribute__((format(printf,3,4))) ;

/**
 * @brief Change a default-context message subscription.
 *
 * @param msg Binding returned by ::IvyBindMsg().
 * @param fmt_regex printf-style regexp format.
 * @param ... Arguments for @p fmt_regex.
 * @return @p msg on success, or NULL on failure.
 *
 * @deprecated Use ::IvyContextChangeMsg().
 *
 * @code{.c}
 * IvyChangeMsg(bind, "^GOODBYE (.*)");
 * @endcode
 */
MsgRcvPtr IvyChangeMsg (MsgRcvPtr msg, const char *fmt_regex, ... )
__attribute__((format(printf,2,3)));

/**
 * @brief Remove a default-context message subscription.
 *
 * @param id Binding returned by ::IvyBindMsg().
 * @return ::IVY_OK on success, or a negative ::IvyStatus.
 *
 * @deprecated Use ::IvyContextUnbindMsg().
 *
 * @code{.c}
 * IvyUnbindMsg(bind);
 * @endcode
 */
int IvyUnbindMsg( MsgRcvPtr id );

/**
 * @brief Send an Ivy protocol error on the current/default context.
 *
 * @param app Peer handle.
 * @param id Application/protocol identifier.
 * @param fmt printf-style error message.
 * @param ... Arguments for @p fmt.
 * @return ::IVY_OK on success, or a negative ::IvyStatus.
 *
 * @deprecated Use ::IvyContextSendError().
 *
 * @code{.c}
 * IvySendError(app, 1, "bad request");
 * @endcode
 */
int IvySendError(IvyClientPtr app, int id, const char *fmt, ... )
__attribute__((format(printf,3,4))) ;

/**
 * @brief Send a die message on the current/default context.
 *
 * @param app Peer handle.
 * @return ::IVY_OK on success, or a negative ::IvyStatus.
 *
 * @deprecated Use ::IvyContextSendDieMsg().
 *
 * @code{.c}
 * IvySendDieMsg(app);
 * @endcode
 */
int IvySendDieMsg(IvyClientPtr app );

/**
 * @brief Send a normal Ivy message on the current/default context.
 *
 * @param fmt_message printf-style message format.
 * @param ... Arguments for @p fmt_message.
 * @return Number of frames accepted locally, or a negative ::IvyStatus if any
 * matching send failed. See ::IvyContextSendMsg() for partial-send semantics.
 *
 * @deprecated Use ::IvyContextSendMsg().
 *
 * @code{.c}
 * IvySendMsg("HELLO %s", name);
 * @endcode
 */
int IvySendMsg( const char *fmt_message, ... )
__attribute__((format(printf,1,2)));

/**
 * @brief Install a direct-message callback on the current/default context.
 *
 * @param callback Callback, or NULL to disable direct messages.
 * @param user_data User pointer passed to @p callback.
 * @return ::IVY_OK on success, or a negative ::IvyStatus.
 *
 * @deprecated Use ::IvyContextBindDirectMsg().
 *
 * @code{.c}
 * IvyBindDirectMsg(on_direct, NULL);
 * @endcode
 */
int IvyBindDirectMsg( MsgDirectCallback callback, void *user_data);

/**
 * @brief Send a direct message on the current/default context.
 *
 * @param app Peer handle.
 * @param id Application-defined direct-message identifier.
 * @param msg Message payload.
 * @return ::IVY_OK on success, or a negative ::IvyStatus.
 *
 * @deprecated Use ::IvyContextSendDirectMsg().
 *
 * @code{.c}
 * IvySendDirectMsg(app, 3, "status");
 * @endcode
 */
int IvySendDirectMsg( IvyClientPtr app, int id, char *msg );

/**
 * @brief Send a ping on the current/default context.
 *
 * @param app Peer handle.
 * @return ::IVY_OK on success, or a negative ::IvyStatus.
 *
 * @deprecated Use ::IvyContextSendPing().
 *
 * @code{.c}
 * IvySetPongCallback(on_pong, ctx);
 * IvySendPing(app);
 * @endcode
 */
int IvySendPing( IvyClientPtr app);

/** @} */

#ifdef __cplusplus
}
#endif

#endif
