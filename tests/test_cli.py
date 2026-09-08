import unittest
from unittest.mock import patch

from cli.dragonbench.main import Client, ClientError, execute, main, parser, render


class FakeClient:
    def __init__(self): self.calls = []
    def request(self, method, path, body=None):
        self.calls.append((method, path, body))
        if path == "/api/v1/status": return {"last_run_id": "abc"}
        if path == "/api/v1/events": return [{"seq": 1}, {"seq": 2}]
        return {"run_id": "abc", "ok": True}


class CliTests(unittest.TestCase):
    def test_machine_output_is_stable_json(self):
        self.assertEqual(render({"b": 1, "a": 2}, True), '{"a":2,"b":1}')

    def test_run_parsing_and_request(self):
        args = parser().parse_args(["run", "cpu_stress", "--duration", "2.5"])
        client = FakeClient()
        execute(args, client)
        self.assertEqual(client.calls[0][2]["workload"], "CPU_STRESS")
        self.assertEqual(client.calls[0][2]["duration_ms"], 2500)

    def test_invalid_duration_fails_before_network(self):
        args = parser().parse_args(["run", "IDLE", "--duration", "0"])
        with self.assertRaises(ClientError): execute(args, FakeClient())

    def test_abort_path(self):
        args = parser().parse_args(["abort", "run-1"])
        client = FakeClient(); execute(args, client)
        self.assertEqual(client.calls[0][:2], ("POST", "/api/v1/runs/run-1/abort"))

    def test_export_marks_external_measurement_separately(self):
        args = parser().parse_args(["export", "abc"])
        result = execute(args, FakeClient())
        self.assertEqual(result["source"], "dut_reported")
        self.assertIsNone(result["external_measurements"])
        self.assertEqual([e["seq"] for e in result["events"]], [1, 2])

    def test_main_nonzero_on_transport_failure(self):
        with patch.object(Client, "request", side_effect=ClientError("offline")):
            self.assertEqual(main(["status"]), 2)


if __name__ == "__main__":
    unittest.main()
