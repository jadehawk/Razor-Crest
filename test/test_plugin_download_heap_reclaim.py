"""Source-contract checks for X3 plugin download heap recovery.

The firmware runtime is not built by this test. It checks that cleanup occurs
only after the download screen has replaced the browsing UI, and that both the
installed-plugin picker and catalog page can still be rebuilt afterward.
"""

from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]
CATALOG = (ROOT / "src/activities/plugins/PluginCatalogActivity.cpp").read_text(encoding="utf-8")
HTTP = (ROOT / "src/network/HttpDownloader.h").read_text(encoding="utf-8")


def function_source(name: str) -> str:
    start = CATALOG.index(f"void PluginCatalogActivity::{name}(")
    end = CATALOG.find("\nvoid PluginCatalogActivity::", start + 1)
    return CATALOG[start:end if end >= 0 else None]


class PluginDownloadHeapReclaimTests(unittest.TestCase):
    def test_reclaim_only_after_download_screen_handoff(self):
        source = function_source("downloadItem")
        handoff = source.index("requestUpdateAndWait();")
        plugins = source.index("std::vector<PluginRef>().swap(installedPlugins);")
        extensions = source.index("std::vector<InstalledExtension>().swap(installedExtensions);")
        transfer = source.index("downloadBundle(item)")
        self.assertLess(handoff, plugins)
        self.assertLess(handoff, extensions)
        self.assertLess(plugins, transfer)
        self.assertLess(extensions, transfer)

    def test_reclaim_is_logged_with_contiguous_block_measurements(self):
        source = function_source("downloadItem")
        self.assertIn("HalMemory::getInternalHeap()", source)
        self.assertIn("download picker reclaim: free=%u->%u max=%u->%u", source)
        self.assertIn("if (HalMemory::getPsramHeap().totalBytes == 0)", source)

    def test_details_drop_idle_tls_before_styled_layout(self):
        source = function_source("showItemDetails")
        self.assertIn("HalMemory::getPsramHeap().totalBytes == 0 && session", source)
        self.assertLess(source.index("session->end();"), source.index("makeUniqueNoThrow<DetailsActivity>"))
        self.assertIn("details TLS reclaim: free=%u->%u max=%u->%u", source)

    def test_cancelling_details_keeps_reusable_client(self):
        source = function_source("showItemDetails")
        self.assertIn("if (result.isCancelled || itemIndex < 0", source)
        self.assertNotIn("session.reset();", source)
        self.assertNotIn("makeUniqueNoThrow<freeink::SecureHttpClient>", source)
        self.assertIn("if (session) session->setReuse(true);", function_source("enterCatalog"))

    def test_catalog_and_picker_can_be_restored_from_sd(self):
        picker = function_source("enterPluginPicker")
        transfer = function_source("downloadItem")
        self.assertIn("installedPlugins = discoverPlugins();", picker)
        self.assertIn("discoverInstalledExtensions();", picker)
        self.assertIn("if (!parseBrowseResponse()) return;", transfer)
        self.assertIn("finishDownload(result);", transfer)

    def test_tls_guard_is_not_relaxed(self):
        self.assertIn("MIN_TLS_FREE_HEAP = 48 * 1024", HTTP)
        self.assertIn("MIN_TLS_MAX_ALLOC = 32 * 1024", HTTP)
        self.assertIn("session.reset();", CATALOG)


if __name__ == "__main__":
    unittest.main(verbosity=2)
