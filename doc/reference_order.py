"""Editorial order for the generated reference; compiler input is unchanged."""

from collections import Counter
import re


CPP_TYPES = [
    "Bus", "Subscription", "SendReport", "AnchoredRegexp", "RuntimeRegexp",
    "DirectSubscription", "EventSubscription", "TimerSubscription", "Every",
    "After", "LoopThread", "ApplicationInfo", "PongTag", "RemoteBindingsTag",
]
CPP_GUIDES = [
    "ivy.hpp", "lifecycle.hpp", "mainloop.hpp", "messages.hpp", "subscriptions.hpp",
    "send.hpp", "results.hpp", "regexp.hpp", "filters.hpp", "callbacks.hpp",
    "timers.hpp", "timer_types.hpp", "applications.hpp", "application_types.hpp",
    "ivy_thread.hpp", "ivy_glib.hpp",
]

CPP_METHODS = [
    ("Initialization and execution", [
        "create", "start", "run", "request_stop", "stop", "take_callback_error", "state"]),
    ("Message subscriptions", ["bind_raw", "bind_convert", "conversion_error", "bind_raw_unanchored"]),
    ("Broadcast messages", ["send", "send_report"]),
    ("Direct messages", ["bind_direct", "send_direct"]),
    ("Filtering", ["set_filters", "add_filter", "remove_filter", "clear_filters"]),
    ("Events and control messages", [
        "bind_event", "send_ping", "set_transport_error_callback", "send_die", "send_error"]),
    ("Application inspection", ["application", "application_info", "applications", "find_application", "application_regexps"]),
    ("Native integration and object ownership", ["native_handle", "~Bus", "Bus", "operator="]),
]

CPP_OBJECT_METHODS = {
    "Subscription": ["unbind", "is_bound", "change", "change_unanchored"],
    "DirectSubscription": ["unbind", "is_bound"],
    "EventSubscription": ["unbind", "is_bound"],
    "TimerSubscription": ["set_period", "unbind", "is_bound"],
    "LoopThread": ["create", "request_stop", "join"],
    "AnchoredRegexp": ["get"],
}

C_FUNCTIONS = [
    ("Initialization and execution", [
        "IvyContextCreate", "IvyInit", "IvyContextStart", "IvyStart",
        "IvyContextMainLoop", "IvyContextRun", "IvyContextRequestStop",
        "IvyContextStop", "IvyStop", "IvyContextDestroy", "IvyTerminate", "IvyGetLastError"]),
    ("Message subscriptions", [
        "IvyContextBindMsg", "IvyBindMsg", "IvyContextChangeMsg", "IvyChangeMsg",
        "IvyContextUnbindMsg", "IvyUnbindMsg", "IvyValidateAnchoredRegexp"]),
    ("Broadcast messages", ["IvyContextSendMsg", "IvySendMsg", "IvyContextSendMsgEx", "IvySendMsgEx"]),
    ("Direct messages", ["IvyContextBindDirectMsg", "IvyBindDirectMsg", "IvyContextSendDirectMsg", "IvySendDirectMsg"]),
    ("Events and control messages", [
        "IvyContextSetBindCallback", "IvySetBindCallback", "IvyContextSetPongCallback",
        "IvySetPongCallback", "IvyContextSendPing", "IvySendPing",
        "IvyContextSetTransportErrorCallback", "IvySetTransportErrorCallback",
        "IvyContextSendDieMsg", "IvySendDieMsg", "IvyContextSendError", "IvySendError"]),
    ("Timers", ["IvyContextTimerRepeatAfter"]),
    ("Application inspection", [
        "IvyContextGetApplicationName", "IvyGetApplicationName", "IvyContextGetApplicationHost",
        "IvyGetApplicationHost", "IvyContextGetApplication", "IvyGetApplication",
        "IvyContextGetApplicationList", "IvyGetApplicationList",
        "IvyContextGetApplicationListBuffer", "IvyGetApplicationListBuffer",
        "IvyContextGetApplicationMessages", "IvyGetApplicationMessages",
        "IvyContextGetApplicationMessagesBuffer", "IvyGetApplicationMessagesBuffer"]),
    ("Advanced integration and compatibility helpers", [
        "IvyContextGetState", "IvyContextIdle", "IvyDefaultApplicationCallback", "IvyDefaultBindCallback"]),
]


def code_only(source: str) -> str:
    """Mask comments/literals, retaining offsets and newlines for declarations."""
    return re.sub(r'//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'',
                  lambda match: re.sub(r"[^\n]", " ", match.group()), source, flags=re.S)


def declarations(source: str) -> tuple[list[str], str]:
    """Split a public scope without detaching comments or inline method bodies."""
    code = code_only(source)
    if re.search(r"^\s*#", code, flags=re.M):
        raise ValueError("Expand preprocessor sections before ordering declarations")
    stack = []
    blocks = []
    start = 0
    for i, character in enumerate(code):
        if character in "([{":
            stack.append(character)
        elif character in ")]}":
            if not stack or stack.pop() != {")": "(", "]": "[", "}": "{"}[character]:
                raise ValueError("Unbalanced declaration scope")
            if character == "}" and not stack and not code[i + 1:].lstrip().startswith(";"):
                # Inline function definitions (LoopThread::create) have no final semicolon.
                blocks.append(source[start:i + 1])
                start = i + 1
        elif character == ";" and not stack:
            blocks.append(source[start:i + 1])
            start = i + 1
    if stack or code[start:].strip():
        raise ValueError("Incomplete declaration scope")
    return blocks, source[start:]


def ordered_declarations(source: str, categories: list, cpp: bool = False, prefix: str = "") -> str:
    blocks, trailing = declarations(source)
    priorities = {name: (section, position, title)
                  for section, (title, names) in enumerate(categories)
                  for position, name in enumerate(names)}

    def priority(block):
        code = code_only(block)
        if re.search(r"\busing\s+\w+\s*=", code):
            return (len(categories), 0, "Callback and result types")
        names = [name for name in priorities
                 if re.search(r"(?<![\w~])" + re.escape(name) + r"\s*\(", code)]
        if len(names) != 1:
            raise ValueError(f"Expected one known declaration, found {names}: {code.strip()}")
        name = names[0]
        if cpp and name == "send" and "IvyClientPtr" in code:
            name = "send_direct"
        return priorities[name]

    ordered = sorted(blocks, key=priority)
    if Counter(ordered) != Counter(blocks):
        raise ValueError("Documentation ordering changed the declarations")
    # Named groups make the summary readable; detailed entries retain this same
    # order through SORT_MEMBER_DOCS=NO. No declaration or contract is rewritten.
    parts = []
    current = None
    for block in ordered:
        section, _, title = priority(block)
        if section == len(categories):
            # Leave aliases in the normal public-types section. Doxygen creates
            # named typedef groups before method groups, regardless of source order.
            if current is not None:
                parts.append("\n/** @} */\n")
                current = None
            parts.append(block)
            continue
        if section != current:
            if current is not None:
                parts.append("\n/** @} */\n")
            parts.append(f"\n/** @name {prefix}{title}\n * @{{\n */\n")
            current = section
        parts.append(block)
    if current is not None:
        parts.append("\n/** @} */\n")
    return "".join(parts) + trailing


def order_c_header(source: str) -> str:
    legacy = []
    for group in ("ivy_context_api", "ivy_legacy_api"):
        opening = re.search(r"/\*\*\s*\n \* @defgroup " + group + r"\b.*?@\{\s*\n \*/", source, re.S)
        if opening is None:
            raise ValueError(f"Missing documented C group: {group}")
        start = opening.end()
        end = source.index("/** @} */", start)
        body = source[start:end]
        if group == "ivy_context_api":
            blocks, trailing = declarations(body)
            legacy = [block for block in blocks if "@ingroup ivy_legacy_api" in block]
            body = "".join(block for block in blocks if block not in legacy) + trailing
            prefix = "C context: "
        else:
            body = "".join(legacy) + body
            prefix = "C legacy: "
        # Member-group names must be unique within this header: Doxygen merges
        # identically named groups even when their enclosing API modules differ.
        source = source[:start] + ordered_declarations(body, C_FUNCTIONS, prefix=prefix) + source[end:]
    return source


def order_cpp_bus(source: str) -> str:
    source = order_cpp_class(source, "Bus", CPP_METHODS)
    for name, methods in CPP_OBJECT_METHODS.items():
        if f"class {name} {{\npublic:\n" in source:
            categories = [("Operations", methods),
                          ("Construction and ownership", [name, "~" + name, "operator="])]
            source = order_cpp_class(source, name, categories, prefix=name + ": ")
    return source


def order_cpp_class(source: str, name: str, categories: list, prefix: str = "") -> str:
    marker = "class " + name + " {\npublic:\n"
    start = source.index(marker) + len(marker)
    end = source.index("\nprivate:\n", start)
    return source[:start] + ordered_declarations(source[start:end], categories, cpp=True, prefix=prefix) + source[end:]


def compound_priority(kind: str, name: str) -> int:
    if kind in {"class", "struct", "union", "interface"}:
        order, name = CPP_TYPES, name.removeprefix("ivy::")
    elif kind == "file":
        order, name = CPP_GUIDES, name.rsplit("/", 1)[-1]
    else:
        return 0
    return order.index(name) if name in order else len(order)
