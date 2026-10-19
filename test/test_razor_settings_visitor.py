#!/usr/bin/env python3
"""Guard Razor-Crest settings through upstream's visitor-based list refactor."""
from pathlib import Path
import re

root = Path(__file__).resolve().parents[1]
settings = (root / "src/SettingsList.cpp").read_text(encoding="utf-8")
storage = (root / "src/CrossPointSettings.cpp").read_text(encoding="utf-8")
advanced = (root / "src/activities/settings/AdvancedSettingsActivity.cpp").read_text(encoding="utf-8")
header = (root / "src/SettingsList.h").read_text(encoding="utf-8")

# The entries must be part of the shared visitor, not only a temporary UI list.
visitor = settings.split("void forEachBaseSetting(SettingVisitor visit, void* context) {", 1)[1].split(
    "std::vector<SettingInfo> getBaseSettingsList(", 1
)[0]
sleep_screen = visitor.index('StrId::STR_SLEEP_SCREEN, &CrossPointSettings::sleepScreen')
timer = visitor.index('StrId::STR_TIMED_SLEEP_SCREEN_REFRESH, &CrossPointSettings::timedSleepScreenRefresh')
sleep_cover = visitor.index('StrId::STR_SLEEP_COVER_MODE, &CrossPointSettings::sleepScreenCoverMode')
assert sleep_screen < timer < sleep_cover, "Timed refresh missing from Display menu ordering"
assert '#if !FREEINK_DEVICE_METALIO_EINK4' in visitor[sleep_screen:timer]
assert '"timedSleepScreenRefresh", StrId::STR_CAT_DISPLAY' in visitor[timer:sleep_cover]
expected_labels = (
    'STR_STATE_OFF', 'STR_1_MINUTE', 'STR_15_MINUTES', 'STR_30_MINUTES',
    'STR_1_HOUR', 'STR_3_HOURS', 'STR_6_HOURS', 'STR_12_HOURS', 'STR_24_HOURS'
)
timer_entry = visitor[timer:sleep_cover]
labels_only = timer_entry.split("{", 1)[1].split("}", 1)[0]
assert tuple(re.findall(r"StrId::(STR_[A-Z0-9_]+)", labels_only)) == expected_labels
assert visitor.count('"timedSleepScreenRefresh"') == 1

for label, field, key in (
    ("STR_PLUGIN_SYSTEM", "pluginsEnabled", "pluginsEnabled"),
    ("STR_PLUGIN_HUB", "pluginHubPromptHidden", "pluginHubPromptHidden"),
):
    expected = f'StrId::{label}, &CrossPointSettings::{field}, "{key}"'
    assert expected in visitor, f"Missing saved descriptor: {expected}"
    assert visitor.count(f'"{key}"') == 1, f"Duplicate saved key: {key}"
assert 'withWebHidden()' in visitor[visitor.index('"pluginsEnabled"'):visitor.index('"pluginHubPromptHidden"')]
assert 'StrId::STR_PLUGIN_SYSTEM' in advanced
assert 'SETTINGS.pluginsEnabled' in advanced and 'SETTINGS.saveToFile()' in advanced

# Both the serializer and deserializer must use the same visitor.
assert storage.count("forEachBaseSetting(visit);") == 2
assert "forEachBaseSetting(SettingVisitor visit, void* context)" in header
assert "forEachBaseSetting(" in settings.split("std::vector<SettingInfo> getBaseSettingsList(", 1)[1]
assert "static const std::vector<SettingInfo> baseList" not in settings
print("Razor-Crest settings visitor: timed refresh + Plugin System keys, menu order, persistence PASS")
