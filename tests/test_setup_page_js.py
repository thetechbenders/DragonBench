import json
import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

from test_contract import _firmware_page

HARNESS = r"""
const PAGE = %s;
const flush = async () => { for (let i = 0; i < 10; i++) await new Promise(r => setImmediate(r)); };

async function scenario(statuses, post) {
  const els = {};
  const el = name => els[name] || (els[name] = {
    textContent: '', value: '', listeners: {},
    addEventListener(type, fn) { this.listeners[type] = fn; },
  });
  const document = { querySelector(sel) { return el(sel.match(/data-role="([^"]+)"/)[1]); } };
  const queue = statuses.slice();
  const intervals = [];
  const fetch = url => {
    if (url === '/api/v1/network/sta') return Promise.resolve({ ok: true, json: () => Promise.resolve(post) });
    const next = queue.shift();
    return Promise.resolve({ ok: true, json: () => Promise.resolve(next) });
  };
  const run = new Function('document', 'fetch', 'setInterval', 'setTimeout', 'clearTimeout', 'AbortController', PAGE);
  run(document, fetch, fn => intervals.push(fn), () => 0, () => {}, class { constructor() { this.signal = {}; } abort() {} });
  await flush();
  el('sta-ssid').value = post.ssid;
  el('sta-password').value = 'replacement-password';
  el('sta-form').listeners.submit({ preventDefault() {} });
  await flush();
  const messages = [el('sta-message').textContent];
  while (queue.length) {
    intervals[0]();
    await flush();
    messages.push(el('sta-message').textContent);
  }
  return messages;
}

const net = (id, result, state, extra) => ({ network: Object.assign({
  sta_config_request_id: id, sta_config_result: result, sta_state: state,
  sta_ssid: 'turvy', sta_last_disconnect_reason: 0 }, extra || {}) });
const post = { state: 'connecting', request_id: 1, ssid: 'turvy' };
const oldConnection = net(0, 'none', 'connected', { sta_ip: '192.168.8.140' });

(async () => {
  const out = {};
  out.replacement_password_fails = await scenario([
    oldConnection,
    oldConnection,
    net(1, 'applied', 'disconnected', { sta_last_disconnect_reason: 15 }),
    net(1, 'applied', 'disconnected', { sta_last_disconnect_reason: 15 }),
  ], post);
  out.replacement_connects = await scenario([
    oldConnection,
    oldConnection,
    net(1, 'applied', 'connecting'),
    net(1, 'applied', 'connected', { sta_ip: '192.168.8.141' }),
  ], post);
  out.apply_fails = await scenario([
    oldConnection,
    oldConnection,
    net(1, 'failed', 'connected', { sta_ip: '192.168.8.140' }),
  ], post);
  out.superseded = await scenario([
    oldConnection,
    net(2, 'applied', 'connected', { sta_ip: '192.168.8.140' }),
  ], post);
  console.log(JSON.stringify(out));
})();
"""


@unittest.skipUnless(shutil.which("node") or os.environ.get("DRAGONBENCH_REQUIRE_NODE"),
                     "Node.js is required to execute the setup page script")
class SetupPageBehaviourTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        html = _firmware_page("setup_page")
        script = html[html.index("<script>") + len("<script>"):html.index("</script>")]
        with tempfile.TemporaryDirectory() as tmp:
            harness = Path(tmp) / "harness.js"
            harness.write_text(HARNESS % json.dumps(script), encoding="utf-8")
            result = subprocess.run(["node", str(harness)], capture_output=True, text=True, timeout=30)
        if result.returncode != 0:
            raise AssertionError(result.stderr)
        cls.messages = json.loads(result.stdout)

    def test_old_connection_is_not_reported_as_success_for_a_new_password(self):
        messages = self.messages["replacement_password_fails"]
        self.assertFalse(any(m.startswith("Connected") for m in messages), messages)
        self.assertIn("wrong password", messages[-1])

    def test_success_is_reported_only_for_the_submitted_request(self):
        messages = self.messages["replacement_connects"]
        self.assertFalse(any(m.startswith("Connected") for m in messages[:-1]), messages)
        self.assertEqual(messages[-1], "Connected to turvy at 192.168.8.141.")

    def test_failed_apply_is_reported(self):
        self.assertIn("Could not apply", self.messages["apply_fails"][-1])

    def test_newer_submission_supersedes(self):
        self.assertIn("newer submission", self.messages["superseded"][-1])


if __name__ == "__main__":
    unittest.main()
