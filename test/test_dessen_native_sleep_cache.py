"""Source contracts for the X3 Dessen guest/native stats ownership boundary.

These fast checks do not compile firmware or replace device memory tests.
"""
import pathlib
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]


class DessenNativeSleepCacheTests(unittest.TestCase):
    def text(self, path):
        return (ROOT / path).read_text(encoding="utf-8")

    def test_native_handoff_frees_guest_before_http_or_pairing(self):
        src = self.text("src/activities/plugins/DessenAppActivity.cpp")
        self.assertIn("void DessenAppActivity::unloadGuestForNativeStats()", src)
        body = src.split("void DessenAppActivity::unloadGuestForNativeStats()", 1)[1]
        body = body.split("void DessenAppActivity::reloadGuestAfterNativeStats()", 1)[0]
        self.assertIn("host.setServices(nullptr);", body)
        self.assertIn("services.reset();", body)
        self.assertIn("memory.reset();", body)
        handoff = src.split("case dessen::Launch::NativeStatsPair:", 1)[1].split(
            "case dessen::Launch::Settings:", 1)[0]
        self.assertLess(handoff.index("unloadGuestForNativeStats();"),
                        handoff.index("silentRestartToDessenOperation(appPath, action);"))
        self.assertIn('"reading-stats-dessen"', src)
        self.assertIn('"reading-statistics-dessen-rust"', src)
        self.assertIn("isStandaloneStatsApp(appPath)", src)
        self.assertIn('const std::string dir = appPath.substr(0, slash);', src)
        self.assertIn('silentRestartToDessen(appPath);', src)
        self.assertIn('pluginhttp::requestToFile(', src)
        self.assertIn('silentRestartToDessen(appPath);', src)  # reboot discards Wi-Fi heap

    def test_guest_network_disabled_in_native_mode(self):
        rust = self.text("experiments/reading-statistics-dessen-rust/readstats.rs")
        self.assertIn("dsn_native_stats_mode()", rust)
        self.assertIn("if native_stats { server.load_cached(); } else { server.fetch(&pairing); }", rust)
        self.assertIn("11 if !native_stats && page == 0", rust)
        self.assertIn("11 if !native_stats && page == 6", rust)
        self.assertIn("native_stats_handoff(", rust)
        self.assertIn("fn native_resume_path(", rust)
        api = self.text("dessen/abi/freeink.api")
        self.assertIn("107 native_stats_mode()", api)
        self.assertIn("108 native_stats(", api)

    def test_sleep_download_is_bounded_and_plugin_local(self):
        src = self.text("src/util/PluginEvents.cpp")
        self.assertIn('strspn(rawDest,', src)
        self.assertIn('std::string(sub.dir) + "/" + rawDest', src)
        self.assertIn('e == Event::SleepEnter && strcmp(sub.name, "reading-statistics") == 0', src)
        self.assertIn('pluginhttp::loadTokenFromFile("/.crosspoint/reading-statistics-token.json"', src)
        self.assertIn('const std::string tmp = dest + ".part";', src)
        self.assertIn("MAX_EVENT_DOWNLOAD", src)

    def test_token_change_invalidates_all_cached_accounts(self):
        src = self.text("src/util/PluginHttp.cpp")
        self.assertIn("void invalidateStatsOverviewCache()", src)
        self.assertIn("for (const char* root : PluginLocations::ROOTS)", src)
        self.assertIn('"reading-stats-dessen"', src)
        self.assertIn('"reading-statistics-dessen-rust"', src)
        self.assertIn("previous != value", src)
        self.assertIn("if (file == kUnifiedStatsToken) invalidateStatsOverviewCache();", src)

    def test_standalone_dessen_pairing_descriptor_requires_no_legacy_plugin(self):
        import json
        descriptor = json.loads(self.text("experiments/reading-statistics-dessen-rust/device.json"))
        self.assertEqual(descriptor["token"]["file"], "/.crosspoint/reading-statistics-token.json")
        self.assertTrue(descriptor["token"]["persist"])
        self.assertEqual(descriptor["auth"]["type"], "device_code")
        self.assertEqual(descriptor["auth"]["request"]["url"], "https://sync.techy-notes.com/api/device/code")
        self.assertEqual(descriptor["auth"]["poll"]["url"], "https://sync.techy-notes.com/api/device/token")
        self.assertEqual(descriptor["browse"]["url"], "https://sync.techy-notes.com/api/device/catalog")
        self.assertNotIn("events", descriptor, "Sleep event must wait until after pairing/fetch hardware trial")
        firmware = self.text("src/activities/plugins/DessenAppActivity.cpp")
        self.assertIn('"reading-statistics-dessen-rust"', firmware)
        self.assertNotIn('PluginLocations::findPluginDir("reading-statistics")', firmware)
        self.assertIn('refreshNativeStatsSnapshot(appPath);', firmware)
        self.assertIn('Storage.exists((dir + "/device.json").c_str())', firmware)
        catalog = self.text("src/activities/plugins/PluginCatalogActivity.cpp")
        self.assertIn('forceInitialCatalog && plugin.name == initialPlugin', catalog)
        self.assertIn('Storage.exists((pluginDir + "/app.bin").c_str())', catalog)
        self.assertIn('PluginLocations::findPluginDir("reading-stats-dessen")', catalog)
        self.assertIn('makeUniqueNoThrow<PluginCatalogActivity>', firmware)
        self.assertIn('false, appName, true);', firmware)
        self.assertNotIn('false, "reading-statistics-dessen-rust", true);', firmware)
        service = self.text("src/activities/plugins/CrosspointServices.h")
        self.assertIn("int nativeStatsMode() override { return 1; }", service)

    def test_silent_reboot_before_and_after_network_restores_dessen(self):
        main = self.text("src/main.cpp")
        self.assertIn("RTC_NOINIT_ATTR char silentDessenAppPath[192];", main)
        self.assertIn("SILENT_REBOOT_TARGET_DESSEN_PAIR = 5", main)
        self.assertIn("SILENT_REBOOT_TARGET_DESSEN_REFRESH = 6", main)
        self.assertIn("void silentRestartToDessenOperation(", main)
        self.assertIn("void silentRestartToDessen(", main)
        self.assertIn("isAllowedDessenStatsPath(appPath)", main)
        self.assertIn('"/reading-stats-dessen/app.bin"', main)
        self.assertIn('"/reading-statistics-dessen-rust/app.bin"', main)
        self.assertIn("makeUniqueNoThrow<DessenAppActivity>(", main)
        self.assertIn("memset(silentDessenAppPath, 0,", main)
        guest = self.text("src/activities/plugins/DessenAppActivity.cpp")
        self.assertIn("silentRestartToDessenOperation(appPath, action);", guest)
        self.assertIn('startActivityForResult(std::move(native)', guest)
        self.assertIn("silentRestartToDessen(appPath);", guest)

    def test_dessen_pairing_returns_immediately_after_token_saved(self):
        catalog = self.text("src/activities/plugins/PluginCatalogActivity.cpp")
        host = self.text("src/activities/plugins/DessenAppActivity.cpp")
        auth = catalog.split("void PluginCatalogActivity::pollAuth()", 1)[1].split(
            "void PluginCatalogActivity::downloadItem", 1)[0]
        self.assertIn("if (!saveToken(newToken))", auth)
        self.assertIn('LOG_INF("PCAT", "Dessen pairing complete: returning to app");', auth)
        self.assertIn("if (forceInitialCatalog) {", auth)
        self.assertLess(auth.index("if (forceInitialCatalog) {"), auth.index("startBrowse();"))
        self.assertIn("finish();", auth.split("if (forceInitialCatalog) {", 1)[1].split("startBrowse();", 1)[0])
        # Ordinary SD plugin catalogs still browse normally after sign-in.
        self.assertIn("      startBrowse();", auth)
        enter = catalog.split("void PluginCatalogActivity::enterCatalog()", 1)[1].split(
            "void PluginCatalogActivity::exitCatalog()", 1)[0]
        self.assertIn("if (forceInitialCatalog && loadToken())", enter)
        self.assertIn("bool PluginCatalogActivity::handleHomeGesture()", catalog)
        self.assertIn("if (!forceInitialCatalog) return false;", catalog)
        self.assertIn('LOG_INF("PCAT", "Dessen pairing cancelled: returning to app");', catalog)
        self.assertIn("Post-pairing overview refresh completed", host)
        self.assertIn("silentRestartToDessen(appPath);", host)
        main = self.text("src/main.cpp")
        self.assertIn("Dessen resume requested", main)
        self.assertIn("Dessen resume: target=", main)

    def test_manual_app_launch_discards_stale_pairing_page(self):
        guest = self.text("src/activities/plugins/DessenAppActivity.cpp")
        header = self.text("src/activities/plugins/DessenAppActivity.h")
        boot = self.text("src/main.cpp")
        rust = self.text("experiments/reading-statistics-dessen-rust/readstats.rs")
        self.assertIn("bool resumeAfterNetwork = false", header)
        self.assertIn("nativeOperation == 0 && !resumeAfterNetwork && isStandaloneStatsApp(appPath)", guest)
        self.assertIn('Storage.remove(marker.c_str())', guest)
        self.assertIn("snapshotTarget == SILENT_REBOOT_TARGET_DESSEN)", boot)
        self.assertIn("if native_stats {", rust)
        self.assertIn("if dsn_fs_read(&path[..n], &mut previous) == 1", rust)
        self.assertIn("native_stats_handoff(if pairing.phase == PairPhase::Paired { 1 } else { 0 }, 0);", rust)
        self.assertNotIn("native_stats_handoff(if pairing.phase == PairPhase::Paired { 1 } else { 0 }, 6);", rust)

    def test_protected_token_reset_is_firmware_owned_and_confirmed(self):
        main = self.text("src/main.cpp")
        firmware = self.text("src/activities/plugins/DessenAppActivity.cpp")
        token_service = self.text("src/util/PluginHttp.cpp")
        protected = self.text("src/network/ProtectedPaths.h")
        sdk = self.text("dessen/abi/freeink.api")
        host = self.text("dessen/freeink/DessenFreeInk.cpp")
        rust = self.text("experiments/reading-statistics-dessen-rust/readstats.rs")
        ui = self.text("experiments/reading-statistics-dessen-rust/sync.rs")
        self.assertIn("SILENT_REBOOT_TARGET_DESSEN_UNPAIR = 7", main)
        self.assertIn("SILENT_REBOOT_TARGET_DESSEN_UNPAIR", main)
        self.assertIn("nativeOperation == 3", firmware)
        self.assertIn("nativeOperation == 3", firmware)
        self.assertIn("pluginhttp::removeTokenFile(kStatsToken)", firmware)
        self.assertIn("silentRestartToDessen(appPath);", firmware)
        self.assertIn("NativeStatsUnpair", firmware)
        self.assertIn("action == 2 ? 7", sdk)
        self.assertIn("kind == 7 ? Launch::NativeStatsUnpair", host)
        self.assertIn("if (file == kUnifiedStatsToken) invalidateStatsOverviewCache();", token_service)
        self.assertIn('"/.crosspoint/reading-statistics-token.json"', protected)
        self.assertIn("confirm_disconnect = true;", rust)
        self.assertIn("native_stats_handoff(2, 0);", rust)
        self.assertIn("fn sync_unpair_confirmation(", ui)
        self.assertIn("fn disconnect_button_bounds(", ui)
        # No direct access to the credential path is exposed to the guest.
        self.assertNotIn("dsn_fs_delete(TOKEN_PATH)", rust)

    def test_x3_sync_render_task_stack_has_bounded_margin(self):
        source = self.text("src/activities/ActivityManager.cpp")
        self.assertIn("#elif FREEINK_DEVICE_X3", source)
        self.assertIn("renderTaskStackBytes = 10240", source)
        self.assertIn("renderTaskStackBytes = 8192", source)
        self.assertIn("renderTaskStackBytes = 16384", source)
        self.assertIn('"ActivityManagerRender"', source)
        # The crash_report.txt task name is shortened to ActivityManager by
        # the FreeRTOS task-name limit. On X3 the render stack had earlier
        # reached 80 bytes free; other platforms keep their existing size.

    def test_month_taps_refresh_without_changing_page(self):
        ui = self.text("experiments/reading-statistics-dessen-rust/preview.rs")
        rust = self.text("experiments/reading-statistics-dessen-rust/readstats.rs")
        self.assertIn("fn overall_month_bounds(", ui)
        self.assertIn("let stale = server.last_synced == 0", ui)
        self.assertIn("let show_icon = paired && (server.icon_revealed || stale);", ui)
        self.assertIn("let (mx, my, mw, mh) = ui.overall_month_bounds();", rust)
        self.assertIn("confirm_month_sync = true;", rust)
        self.assertIn("if syncing_month {", rust)
        self.assertIn("native_stats_handoff(1, 0);", rust)
        self.assertIn("ui.overall_sync_confirmation();", rust)
        self.assertIn("b\"Syncing...\"", ui)
        self.assertNotIn("THIS MONTH - LOCAL FALLBACK", ui)


if __name__ == "__main__":
    unittest.main()
