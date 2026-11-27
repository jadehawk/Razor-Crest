"""Source contract: Dessen network calls must not enter transport while offline."""
import re
import unittest
from pathlib import Path

SOURCE = (Path(__file__).resolve().parents[1] /
          "src/activities/plugins/CrosspointServices.cpp").read_text()


class DessenNetworkGuardTests(unittest.TestCase):
    def check_guard(self, operation, transport):
        case = SOURCE.split(f"case Op::{operation}: {{", 1)[1].split("case Op::", 1)[0]
        guard = re.search(
            r"if\s*\(WiFi\.status\(\)\s*!=\s*WL_CONNECTED\)\s*\{\s*"
            r"result_\s*=\s*DSN_ERR_IO;\s*break;\s*\}", case
        )
        self.assertIsNotNone(guard)
        self.assertLess(guard.end(), case.index(transport))

    def test_http_requires_connected_wifi(self):
        self.check_guard("Net", "HttpDownloader::request(")

    def test_fetch_requires_connected_wifi(self):
        self.check_guard("Fetch", "HttpDownloader::downloadRestricted(")

    def test_tls_guard_uses_psram_aware_reserve(self):
        policy = SOURCE.split("bool tlsHeapAvailable() {", 1)[1].split("}", 1)[0]
        self.assertRegex(
            policy,
            r"HalMemory::getPsramHeap\(\)\.totalBytes\s*>\s*0\s*\?\s*"
            r"HttpDownloader::MIN_TLS_MAX_ALLOC_PSRAM\s*:\s*HttpDownloader::MIN_TLS_MAX_ALLOC",
        )
        self.assertRegex(
            policy,
            r"ESP\.getFreeHeap\(\)\s*>=\s*HttpDownloader::MIN_TLS_FREE_HEAP\s*&&\s*"
            r"ESP\.getMaxAllocHeap\(\)\s*>=\s*minBlock",
        )

    def test_both_https_transports_enforce_shared_tls_guard(self):
        for operation, transport in (
            ("Net", "HttpDownloader::request("),
            ("Fetch", "HttpDownloader::downloadRestricted("),
        ):
            with self.subTest(operation=operation):
                case = SOURCE.split(f"case Op::{operation}: {{", 1)[1].split("case Op::", 1)[0]
                guard = re.search(
                    r'strncasecmp\([^\n]+?"https:",\s*6\)\s*==\s*0\s*&&\s*!tlsHeapAvailable\(\)\)\s*\{'
                    r'.*?result_\s*=\s*DSN_ERR_IO;\s*break;\s*\}', case, re.S
                )
                self.assertIsNotNone(guard)
                self.assertLess(guard.end(), case.index(transport))

    def test_error_completes_pending_operation(self):
        service = SOURCE.split("bool CrosspointServices::serviceAsyncWork()", 1)[1]
        self.assertRegex(service, r"\}\s*(?:#endif\s*)?done_ = true;\s*return true;")
        poll = SOURCE.split("int CrosspointServices::poll()", 1)[1].split("bool CrosspointServices::serviceAsyncWork", 1)[0]
        self.assertRegex(poll, r"pending_ = Op::None;\s*return result_;")


if __name__ == "__main__":
    unittest.main()
