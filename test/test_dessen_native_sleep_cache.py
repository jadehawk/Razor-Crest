"""Firmware storage and memory contracts retained after generic Dessen migration.

These fast checks do not compile firmware or replace device memory tests.
"""
import pathlib
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]


class DessenNativeSleepCacheTests(unittest.TestCase):
    def text(self, path):
        return (ROOT / path).read_text(encoding="utf-8")

    def test_dessen_picker_releases_borrowed_rows_before_launch(self):
        src = self.text("src/activities/plugins/PluginCatalogActivity.cpp")
        launch = src.split("auto appActivity = makeUniqueNoThrow<DessenAppActivity>", 1)[1].split(
            "const auto action = PluginLocations::pickerAction", 1)[0]
        self.assertLess(launch.index("pickerReturnName = plugin.name;"),
                        launch.index("decltype(installedPlugins){}.swap(installedPlugins);"))
        self.assertLess(launch.index("releaseRows();"), launch.index("decltype(rowItems){}.swap(rowItems);"))
        self.assertLess(launch.index("decltype(rowItems){}.swap(rowItems);"),
                        launch.index("decltype(installedPlugins){}.swap(installedPlugins);"))
        self.assertLess(launch.index("decltype(installedExtensions){}.swap(installedExtensions);"),
                        launch.index("startActivityForResult(std::move(appActivity)"))
        self.assertLess(launch.index("enterPluginPicker();"), launch.index("pickerSuspendedForDessen = false;"))
        self.assertIn("if (pickerSuspendedForDessen) return;", src)
        self.assertIn("Dessen picker release: free=%u->%u max_block=%u->%u", launch)

    def test_plugin_launches_prefer_dessen_before_native_dashboard(self):
        picker = self.text("src/activities/plugins/PluginCatalogActivity.cpp")
        dispatch = picker.split("void PluginCatalogActivity::activateIndex(", 1)[1].split(
            "const auto action = PluginLocations::pickerAction", 1)[0]
        self.assertLess(dispatch.index("#if CROSSPOINT_DESSEN_APPS"),
                        dispatch.index("makeUniqueNoThrow<DessenAppActivity>"))
        self.assertLess(dispatch.index("makeUniqueNoThrow<DessenAppActivity>"),
                        dispatch.index("openNativeDashboard(plugin.manifestPath)"))
        self.assertEqual(dispatch.count("!(forceInitialCatalog && plugin.name == initialPlugin)"), 2)

        home = self.text("src/activities/ActivityManager.cpp")
        shortcut = home.split("void ActivityManager::goToPluginDashboard(", 1)[1].split(
            "bool ActivityManager::goToReader(", 1)[0]
        self.assertLess(shortcut.index("#if CROSSPOINT_DESSEN_APPS"),
                        shortcut.index("makeUniqueNoThrow<DessenAppActivity>"))
        self.assertLess(shortcut.index("makeUniqueNoThrow<DessenAppActivity>"),
                        shortcut.index("StatsDashboardActivity::supportsManifest"))

    def test_explicit_dual_mode_fallback_skips_only_installed_dessen_apps(self):
        src = self.text("src/util/PluginEvents.cpp")
        scan = src.split("void refreshSubscriptions()", 1)[1].split("bool anySubscriber", 1)[0]
        self.assertIn('filter["events_fallback_only"] = true;', scan)
        gate = scan.split('if (doc["events_fallback_only"].is<bool>()', 1)[1]
        self.assertIn('doc["events_fallback_only"].as<bool>()', gate)
        self.assertIn('Storage.exists((entry.dir + "/app.bin").c_str())', gate)
        self.assertLess(gate.index("continue;"), gate.index("uint8_t mask = 0;"))
        self.assertIn("#if CROSSPOINT_DESSEN_APPS", scan)
        self.assertRegex(gate, r"continue;\s*#endif")
        self.assertNotIn("Storage.remove", scan)

    def test_empty_wifi_scans_retry_before_hidden_network(self):
        header = self.text("src/activities/network/WifiSelectionActivity.h")
        src = self.text("src/activities/network/WifiSelectionActivity.cpp")
        self.assertIn("MAX_EMPTY_SCAN_RETRIES = 2", header)
        self.assertIn("if (!retry) emptyScanRetries = 0;", src)
        self.assertIn("if (emptyScanRetries >= MAX_EMPTY_SCAN_RETRIES) return false;", src)
        self.assertIn("if (retryEmptyScan()) return;", src)
        self.assertIn("if (realNetworkCount == 0 && retryEmptyScan()) return;", src)
        self.assertIn("startWifiScan(autoConnecting && !manualRequested, true);", src)

    def test_sleep_download_is_bounded_and_plugin_local(self):
        src = self.text("src/util/PluginEvents.cpp")
        self.assertIn('strspn(rawDest,', src)
        self.assertIn('std::string(sub.dir) + "/" + rawDest', src)
        self.assertIn('e == Event::SleepEnter && strcmp(sub.name, "reading-statistics") == 0', src)
        self.assertIn('pluginhttp::loadTokenFromFile("/.crosspoint/reading-statistics-token.json"', src)
        self.assertIn('const std::string tmp = dest + ".part";', src)
        self.assertIn("MAX_EVENT_DOWNLOAD", src)

    def test_sleep_uses_generic_guest_storage_operations(self):
        service = self.text("src/activities/plugins/CrosspointServices.cpp")
        sleep = self.text("src/activities/boot_sleep/SleepActivity.cpp")
        app = self.text("experiments/reading-statistics-dessen-rust/sleep.rs")
        capture = service.split("int CrosspointServices::screenshot(",1)[1].split("int CrosspointServices::fsRename(",1)[0]
        self.assertIn("permissions_.allows(path, DessenPermissions::Access::Write)",capture)
        self.assertIn("permissions_.isSharedCredential(path)",capture)
        self.assertIn("Storage.exists(path)",capture)
        self.assertIn("ScreenshotUtil::saveFramebufferAsBmp(path, renderer)",capture)
        move = service.split("int CrosspointServices::fsRename(",1)[1].split("return 0;",1)[0]
        self.assertIn("permissions_.allows(source, DessenPermissions::Access::Read)",move)
        self.assertIn("permissions_.allows(source, DessenPermissions::Access::Delete)",move)
        self.assertIn("permissions_.allows(dest, DessenPermissions::Access::Write)",move)
        self.assertIn("Storage.exists(dest)",move)
        self.assertIn("Storage.rename(source, dest)",move)
        self.assertIn('renderCustomSleepBmp("/sleep.bmp")',sleep)
        self.assertNotIn("ReadingStatsSleepSource",sleep)
        self.assertNotIn("sleep-server.bmp",sleep)
        self.assertNotIn("sleep-device.bmp",sleep)
        self.assertIn("dsn_screenshot(temp)",app)
        self.assertIn("dsn_fs_rename(SLEEP_ROOT,SLEEP_BACKUP)",app)
        self.assertIn("dsn_fs_rename(SLEEP_BACKUP,SLEEP_ROOT)",app)

    def test_token_change_invalidates_all_cached_accounts(self):
        src = self.text("src/util/PluginHttp.cpp")
        self.assertIn("void invalidateStatsOverviewCache()", src)
        self.assertIn("for (const char* root : PluginLocations::ROOTS)", src)
        self.assertNotIn('"reading-stats-dessen"', src)
        self.assertNotIn('"reading-statistics-dessen-rust"', src)
        self.assertIn("previous != value", src)
        self.assertIn("if (file == kUnifiedStatsToken) invalidateStatsOverviewCache();", src)

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

    def test_x3_covers_reuse_existing_bounded_bitmap_cache(self):
        source = self.text("src/activities/plugins/CrosspointServices.cpp")
        cached = source.split("static std::string cachedBookThumbnail(", 1)[1].split("int CrosspointServices::bookCover(", 1)[0]
        self.assertIn("getThumbBmpPath()", cached)
        self.assertIn('upstreamPrefix = "thumb_"', cached)
        self.assertIn("len <= prefixLength + suffix.size()", cached)
        self.assertNotIn("reading-stats/covers", cached)
        self.assertIn("dir.openNextFile()", cached)
        self.assertIn("sourceWidth > 1024", cached)
        self.assertIn("rowBytes > 4096", cached)
        self.assertIn("bmp->readRowGray(sourceY, gray, row)", cached)
        self.assertNotIn("generateThumbBmp", cached)
        device_path = source.split("int CrosspointServices::bookCover(", 1)[1].split("#else", 1)[0]
        self.assertIn("cachedBookThumbnail(full, height)", device_path)
        self.assertIn("packCachedBookCover(cached, height", device_path)

    def test_month_taps_refresh_without_changing_page(self):
        ui = self.text("experiments/reading-statistics-dessen-rust/preview.rs")
        rust = self.text("experiments/reading-statistics-dessen-rust/readstats.rs")
        self.assertIn("fn overall_month_bounds(", ui)
        self.assertIn("let stale = server.stale(dsn_time())", ui)
        self.assertIn("let show_icon = !self.sleep_render && paired && (server.icon_revealed || stale);", ui)
        self.assertIn("let (mx, my, mw, mh) = ui.overall_month_bounds();", rust)
        self.assertIn("confirm_month_sync = true;", rust)
        self.assertIn("if syncing_month {", rust)
        self.assertIn("let refreshed = server.fetch(&pairing,&mut home.cover);", rust)
        self.assertIn("if refreshed { page = 0; }", rust)
        self.assertIn("ui.overall_sync_confirmation();", rust)
        self.assertIn("b\"Syncing...\"", ui)
        self.assertNotIn("THIS MONTH - LOCAL FALLBACK", ui)


if __name__ == "__main__":
    unittest.main()
