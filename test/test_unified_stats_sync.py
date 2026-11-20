"""Non-build contract checks for optional Reading Statistics Server Sync integration."""
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[1]
TOKEN = "/.crosspoint/reading-statistics-token.json"


def source(rel: str) -> str:
    return (ROOT / rel).read_text(encoding="utf-8")


class UnifiedStatsFirmwareContract(unittest.TestCase):
    def test_seventh_page_launches_existing_plugin_catalog(self):
        h = source("src/activities/plugins/StatsDashboardActivity.h")
        c = source("src/activities/plugins/StatsDashboardActivity.cpp")
        self.assertIn("std::array<Page, 7> pages", h)
        self.assertIn('widget.type != "server_sync"', c)
        self.assertIn('widget.type == "server_sync"', c)
        self.assertIn("makeUniqueNoThrow<PluginCatalogActivity>", c)
        self.assertIn("false, kUnifiedPluginName", c)
        self.assertIn("migrateLegacySyncState(manifestPath)", c)
        self.assertIn("if (serverSyncPage())", c)

    def test_persistent_token_has_scoped_access_and_is_not_generic_file(self):
        paths = source("src/network/ProtectedPaths.h")
        transport = source("src/util/PluginHttp.cpp")
        picker = source("src/activities/plugins/PluginCatalogActivity.cpp")
        events = source("src/util/PluginEvents.cpp")
        for text in (paths, transport, picker, events):
            self.assertIn(TOKEN, text)
        self.assertIn("pluginDir == PluginLocations::findPluginDir", picker)
        self.assertIn('strcmp(sub.name, "reading-statistics") != 0', events)
        self.assertIn("manifest.configFile == statsToken", picker)
        self.assertIn("out.configFile == statsToken", events)

    def test_non_destructive_migration_cannot_mix_guest_sessions(self):
        c = source("src/activities/plugins/StatsDashboardActivity.cpp")
        self.assertIn('"/reading-stats-sync"', c)
        self.assertIn('"/token.json"', c)
        self.assertIn('"/events.jsonl"', c)
        self.assertIn("!Storage.exists(kPersistentSyncToken)", c)
        self.assertIn("legacyToken == unifiedToken", c)
        self.assertIn("if (sameAccount && !Storage.exists(newQueue.c_str()))", c)
        # Only the old credential is read; its files are never removed.
        migrate = c.split("void migrateLegacySyncState(", 1)[1].split("constexpr int kMargin", 1)[0]
        self.assertNotIn("Storage.remove(", migrate)
        self.assertNotIn("Storage.removeDir(", migrate)
        self.assertNotIn("Storage.rename(", migrate)

    def test_standard_dashboard_and_sleep_overview_unchanged(self):
        c = source("src/activities/plugins/StatsDashboardActivity.cpp")
        self.assertIn('if (genericCardDashboard)', c)
        self.assertIn('pages[0].title = "Book Statistics"', c)
        self.assertIn('drawReadingSnapshot(', c)
        self.assertIn('drawLegacyImport(', c)
        self.assertIn('const char* confirmLabel', c)


if __name__ == "__main__":
    unittest.main()
