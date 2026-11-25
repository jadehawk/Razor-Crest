"""Firmware ownership boundaries. Executable continuation tests live in dessen."""
from pathlib import Path
import unittest
ROOT = Path(__file__).resolve().parents[1]
APP = (ROOT/"src/activities/plugins/DessenAppActivity.cpp").read_text()
NET = (ROOT/"src/activities/plugins/CrosspointServices.cpp").read_text()
HOST = (ROOT/"dessen/freeink/DessenFreeInk.cpp").read_text()

class SuspensionTests(unittest.TestCase):
    def test_release_only_after_durable_authenticated_snapshot(self):
        suspend = APP.split("bool DessenAppActivity::suspendNetwork()",1)[1].split("bool DessenAppActivity::resumeNetwork()",1)[0]
        stages = ["transferSnapshot(", "file.close()", "detachNetworkBuffers(", "host.detachMemory()", "memory.reset()"]
        positions = [suspend.index(stage) for stage in stages]
        self.assertEqual(positions, sorted(positions))
        self.assertIn("wc_AesGcmEncrypt", APP)
        self.assertIn("wc_AesGcmDecrypt", APP)
        self.assertNotIn("file.write(memory", APP)

    def test_guest_memory_returns_only_after_clean_reboot(self):
        stage = APP.split("bool DessenAppActivity::resumeNetwork()",1)[1].split("bool DessenAppActivity::restoreContinuation()",1)[0]
        self.assertNotIn("makeUniqueNoThrow", stage)
        self.assertNotIn("host.attachMemory", stage)
        self.assertLess(stage.index("transferSnapshot("), stage.index("rebootTicket.magic = RESUME_MAGIC"))
        self.assertLess(stage.index("rebootTicket.magic = RESUME_MAGIC"), stage.index("silentRestartToDessen(appPath)"))
        self.assertIn("clearTicket(rebootTicket)", APP)
        self.assertIn("esp_reset_reason() != ESP_RST_SW", APP)

    def test_transport_starts_only_after_clean_network_boot(self):
        suspend = APP.split("bool DessenAppActivity::suspendNetwork()", 1)[1].split("bool DessenAppActivity::resumeNetwork()", 1)[0]
        stages = ["exportNetworkRequest(", "requestFile.close()", "host.detachMemory()", "memory.reset()", "services.reset()", "rebootTicket.magic = NETWORK_MAGIC", "silentRestartToDessen(appPath)"]
        self.assertEqual([suspend.index(s) for s in stages], sorted(suspend.index(s) for s in stages))
        self.assertNotIn("startNetworkWork()", suspend)
        boot = APP.split("void DessenAppActivity::onEnter()", 1)[1].split("void DessenAppActivity::startAuthorizedApp()", 1)[0]
        self.assertLess(boot.index("memcmp(services->permissionDigest()"), boot.index("importNetworkRequest("))
        self.assertLess(boot.index("readSnapshot(REQUEST_SNAPSHOT"), boot.index("importNetworkRequest("))
        self.assertLess(boot.index("importNetworkRequest("), boot.index("startNetworkWork()"))
        self.assertIn("haveResume = !haveNetwork", APP)
        self.assertIn("activeTicket.key, true, 1", APP)

    def test_request_is_bounded_and_reauthorized(self):
        request = NET.split("bool CrosspointServices::importNetworkRequest(", 1)[1].split("bool CrosspointServices::startSuspendedWifi()", 1)[0]
        self.assertIn("size > MAX_NETWORK_REQUEST", request)
        self.assertIn("permissions_.allowsHandoff()", request)
        self.assertIn("cap > guestSize - offset", request)
        self.assertIn("netStart(first.c_str()", request)
        self.assertIn("fetchStart(first.c_str()", request)

    def test_response_is_bound_to_origin_and_manifest(self):
        restore = APP.split("bool DessenAppActivity::restoreContinuation()",1)[1].split("#else",1)[0]
        self.assertIn("appPath != activeTicket.appPath", restore)
        self.assertIn("memcmp(services->permissionDigest()", restore)
        self.assertIn("count <= size - offset", restore)
        self.assertLess(restore.index("readSnapshot(VM_SNAPSHOT"), restore.index("host.resumeContinuation"))
        self.assertLess(restore.index("host.resumeContinuation"), restore.index("clearTicket(activeTicket)"))

    def test_runtime_has_no_application_business_logic(self):
        for text in (APP, NET, HOST):
            for forbidden in ("reading-statistics", "sync.techy-notes.com", "NativeStats", "native_stats", "overviewRequest"):
                self.assertNotIn(forbidden, text)

    def test_unloaded_guest_cannot_poll_or_execute(self):
        self.assertLess(HOST.index("if (!vm_.mem) return state_"), HOST.index("services_->poll()"))
        self.assertIn("if (networkDetached_) return dessen::kPending", NET)
        self.assertIn("if (!services || !memory", APP)

    def test_timer_requires_application_opt_in(self):
        self.assertIn("host.timerInterval()", APP)
        self.assertIn("suppressTimerFrameRestore = true", APP)
        self.assertIn("!skipFrameRestore", APP)

    def test_worker_owns_io_until_completion_or_cancel(self):
        render = APP.split("void DessenAppActivity::render(", 1)[1]
        self.assertIn("networkRunning.load(std::memory_order_acquire)", render)
        worker = APP.split("void DessenAppActivity::runNetworkWork(", 1)[1].split("bool DessenAppActivity::startNetworkWork(", 1)[0]
        self.assertLess(worker.index("serviceAsyncWork()"), worker.index("networkDone.store(true"))
        shutdown = APP.split("void DessenAppActivity::stopNetworkWork()", 1)[1].split("void DessenAppActivity::runNetworkWork(", 1)[0]
        self.assertLess(shutdown.index("cancelNetwork()"), shutdown.index("while (!networkDone"))
        self.assertIn("networkCancelled_.load", NET)
        transport = (ROOT/"src/network/HttpDownloader.cpp").read_text()
        self.assertIn("cancelFlag->load", transport)
        self.assertIn("millis() - started >= 60000", transport)

    def test_interrupted_staging_is_owned_and_cleaned(self):
        self.assertIn("cleanupDessenTemporaryFiles()", APP)
        self.assertIn('"/.crosspoint/dsn-write.tmp"', APP)
        self.assertIn('"/.crosspoint/dsn-download.tmp"', APP)
        self.assertIn('normalized.ends_with(".part")', APP)
        self.assertIn("cleanupDownload()", NET)

if __name__ == "__main__":
    unittest.main()
