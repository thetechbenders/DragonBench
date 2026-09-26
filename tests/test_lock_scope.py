"""Static guard: firmware must never hold state_lock across blocking I/O.

state_lock protects product state and is taken by should_abort() inside every
workload loop and by emit_event(). Holding it across an HTTP response or socket
call lets a slow or dead network peer stall running workloads, aborts and event
producers. This test walks main.c and fails if any such call can execute while
state_lock is held.

The walker is brace-aware so the early-release pattern used by handlers,

    xSemaphoreTake(state_lock, ...);
    if (busy) { xSemaphoreGive(state_lock); return send_json(...); }
    ...
    xSemaphoreGive(state_lock);

is understood: a Give inside a nested block releases the lock only until that
block closes. Comments and string/char literals are blanked first.
"""

import re
import unittest
from pathlib import Path

ROOT = Path(__file__).parents[1]
MAIN_C = ROOT / "firmware/targets/esp32s3/main/main.c"

# Calls that transmit, receive or otherwise block on the network.
BLOCKING_CALLS = (
    "httpd_resp_send", "httpd_resp_send_chunk", "httpd_resp_sendstr",
    "httpd_resp_sendstr_chunk", "httpd_resp_send_err", "httpd_req_recv",
    "send_json", "send", "sendto", "recv", "recvfrom", "connect",
    "getaddrinfo",
)

TOKEN = re.compile(
    r"(?P<take>\bxSemaphoreTake\s*\(\s*state_lock\b)"
    r"|(?P<give>\bxSemaphoreGive\s*\(\s*state_lock\b)"
    r"|(?P<call>\b(?:" + "|".join(BLOCKING_CALLS) + r")\s*\()"
    r"|(?P<open>\{)|(?P<close>\})"
)


def _blank_comments_and_literals(src: str) -> str:
    """Replace comments and string/char literal contents with spaces,
    preserving newlines so reported line numbers stay accurate."""
    out, i, n = [], 0, len(src)
    while i < n:
        c = src[i]
        if src.startswith("//", i):
            j = src.find("\n", i)
            j = n if j < 0 else j
            out.append(" " * (j - i)); i = j
        elif src.startswith("/*", i):
            j = src.find("*/", i + 2)
            j = n if j < 0 else j + 2
            out.append(re.sub(r"[^\n]", " ", src[i:j])); i = j
        elif c in "\"'":
            j = i + 1
            while j < n and src[j] != c:
                j += 2 if src[j] == "\\" else 1
            j = min(j + 1, n)
            out.append(c + re.sub(r"[^\n]", " ", src[i + 1:j - 1]) + c); i = j
        else:
            out.append(c); i += 1
    return "".join(out)


def lock_scope_violations(src: str):
    """Return [(line, call)] for blocking calls reachable with state_lock held,
    plus structural problems (nested take, unmatched give, lock left held)."""
    code = _blank_comments_and_literals(src)
    problems = []
    depth = 0
    held = False
    take_depth = 0
    released_in_block = None  # depth of the block whose Give released the lock

    def line_of(pos):
        return code.count("\n", 0, pos) + 1

    for m in TOKEN.finditer(code):
        kind = m.lastgroup
        if kind == "open":
            depth += 1
        elif kind == "close":
            depth -= 1
            if released_in_block is not None and depth < released_in_block:
                released_in_block = None  # branch ended; lock held again
            if held and depth < take_depth:
                problems.append((line_of(m.start()), "state_lock still held at end of block"))
                held = False
        elif kind == "take":
            if held and released_in_block is None:
                problems.append((line_of(m.start()), "nested xSemaphoreTake(state_lock)"))
            held, take_depth, released_in_block = True, depth, None
        elif kind == "give":
            if not held:
                problems.append((line_of(m.start()), "xSemaphoreGive(state_lock) without take"))
            elif depth > take_depth:
                released_in_block = depth
            else:
                held, released_in_block = False, None
        elif kind == "call" and held and released_in_block is None:
            name = re.match(r"\w+", m.group("call")).group(0)
            problems.append((line_of(m.start()), name))
    return problems


class LockScopeTest(unittest.TestCase):
    def test_firmware_never_holds_state_lock_across_network_io(self):
        problems = lock_scope_violations(MAIN_C.read_text())
        self.assertEqual(problems, [], "state_lock held across blocking I/O: %r" % problems)

    def test_events_get_snapshots_then_sends(self):
        src = _blank_comments_and_literals(MAIN_C.read_text())
        body = re.search(r"static esp_err_t events_get\(httpd_req_t \*req\) \{(.*?)\n\}", src, re.S)
        self.assertIsNotNone(body, "events_get not found")
        body = body.group(1)
        give = body.find("xSemaphoreGive(state_lock")
        first_send = body.find("httpd_resp_send_chunk")
        self.assertGreater(give, 0)
        self.assertGreater(first_send, give, "events_get must release state_lock before sending")

    # ---- the walker itself --------------------------------------------------

    def test_detects_send_while_held(self):
        src = """
        static esp_err_t h(httpd_req_t *req) {
            xSemaphoreTake(state_lock, portMAX_DELAY);
            for (size_t i = 0; i < n; ++i) {
                httpd_resp_send_chunk(req, x, 1);
            }
            xSemaphoreGive(state_lock);
            return httpd_resp_send_chunk(req, NULL, 0);
        }"""
        self.assertEqual([name for _, name in lock_scope_violations(src)], ["httpd_resp_send_chunk"])

    def test_early_release_branch_is_allowed(self):
        src = """
        static esp_err_t h(httpd_req_t *req) {
            xSemaphoreTake(state_lock, portMAX_DELAY);
            if (busy) {
                xSemaphoreGive(state_lock); return send_json(req, o, 409);
            }
            state = 1;
            xSemaphoreGive(state_lock);
            return send_json(req, o, 201);
        }"""
        self.assertEqual(lock_scope_violations(src), [])

    def test_send_after_branch_but_before_final_give_is_caught(self):
        src = """
        static esp_err_t h(httpd_req_t *req) {
            xSemaphoreTake(state_lock, portMAX_DELAY);
            if (busy) { xSemaphoreGive(state_lock); return send_json(req, o, 409); }
            send(fd, buf, n, 0);
            xSemaphoreGive(state_lock);
        }"""
        self.assertEqual([name for _, name in lock_scope_violations(src)], ["send"])

    def test_comments_and_strings_are_ignored(self):
        src = """
        static void h(void) {
            xSemaphoreTake(state_lock, portMAX_DELAY);
            /* httpd_resp_send(req, x, 1); */  // send(fd, b, 1, 0);
            const char *s = "httpd_resp_sendstr(";
            xSemaphoreGive(state_lock);
        }"""
        self.assertEqual(lock_scope_violations(src), [])

    def test_lock_left_held_is_reported(self):
        src = "static void h(void) { xSemaphoreTake(state_lock, portMAX_DELAY); x = 1; }"
        self.assertEqual(len(lock_scope_violations(src)), 1)


if __name__ == "__main__":
    unittest.main()
