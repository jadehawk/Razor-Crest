"""Stage 1 proposed Home layout V1 contract checks (host-only, NOT firmware parser).

Validates reference fixture syntax and obvious geometry conflicts without building
firmware. Production capabilities MUST NOT be advertised based on this test alone.
Run: python test/test_home_layout_contract.py
"""

import copy
import json
from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parent / "fixtures" / "home_layout_v1"
KEY = re.compile(r"[a-z][a-z0-9_-]{0,31}\Z")
NAME = re.compile(r"[a-z0-9][a-z0-9._-]{0,63}\Z")
MAX_BYTES = 8192
METRICS = {
    "active_days", "last_7_seconds", "sessions", "total_seconds",
    "qualified_turns", "seconds_per_turn", "latest_day_seconds",
}


class ContractError(ValueError):
    pass


def fail(message):
    raise ContractError(message)


def keys(data, required, optional=(), context="object"):
    if not isinstance(data, dict):
        fail(f"{context}: expected object")
    unexpected = set(data).difference(set(required) | set(optional))
    missing = set(required).difference(data)
    if missing or unexpected:
        fail(f"{context}: missing {sorted(missing)}, unknown {sorted(unexpected)}")


def integer(value, lo, hi, name):
    if type(value) is not int or not lo <= value <= hi:
        fail(f"{name}: expected integer in [{lo}, {hi}]")
    return value


def identity(value, name, pattern=KEY):
    if not isinstance(value, str) or not pattern.fullmatch(value):
        fail(f"{name}: invalid identifier")


def parse_unique_pairs(pairs):
    obj = {}
    for key, val in pairs:
        if key in obj:
            fail(f"duplicate JSON key: {key}")
        obj[key] = val
    return obj


def load_fixture(name):
    raw = (ROOT / f"{name}.json").read_bytes()
    if len(raw) > MAX_BYTES:
        fail("layout JSON exceeds 8192 UTF-8 bytes")
    return json.loads(raw.decode("utf-8"), object_pairs_hook=parse_unique_pairs)


def validate_widget(widget, first_row=False):
    keys(widget, {"type"}, {
        "id", "panel", "scene", "carousel", "variant", "source",
        "row_height", "overflow", "plugin", "widget", "fallback", "field",
    }, "widget")
    kind = widget["type"]
    allowed = {
        "native.header": {"type", "id", "panel"},
        "theme.scene": {"type", "id", "panel", "scene"},
        "native.book": {"type", "id", "panel", "carousel", "variant"},
        "native.shortcuts": {"type", "id", "panel", "source", "row_height", "overflow"},
        "native.clock": {"type", "id", "panel"},
        "native.metric": {"type", "id", "panel", "field"},
        "plugin.widget": {"type", "id", "panel", "plugin", "widget", "fallback"},
        "empty": {"type", "id", "panel"},
    }
    if kind not in allowed:
        fail(f"widget: unsupported type {kind!r}")
    extra = set(widget) - allowed[kind]
    if extra:
        fail(f"widget {kind}: unsupported options {sorted(extra)}")
    if "id" in widget:
        identity(widget["id"], "widget.id")
    if widget.get("panel", "opaque") not in ("opaque", "transparent"):
        fail("widget.panel: unsupported")
    if kind == "native.header" and not first_row:
        fail("native.header is only permitted in first row")
    if kind != "native.header" and first_row:
        fail("first row must contain native.header")
    if kind == "theme.scene":
        if "scene" not in widget:
            fail("theme.scene requires scene")
        identity(widget["scene"], "scene", NAME)
    if kind == "native.book":
        if "carousel" in widget and type(widget["carousel"]) is not bool:
            fail("book.carousel must be boolean")
        if widget.get("variant", "cover") not in {"cover", "outlined"}:
            fail("book.variant unsupported")
    if kind == "native.shortcuts":
        if widget.get("source", "home_shortcuts") != "home_shortcuts":
            fail("shortcuts.source unsupported")
        if widget.get("overflow", "scroll") != "scroll":
            fail("shortcuts must remain scrollable")
        if "row_height" in widget:
            integer(widget["row_height"], 24, 100, "row_height")
    if kind == "native.metric":
        if widget.get("field") not in METRICS:
            fail("metric.field unsupported")
    if kind == "plugin.widget":
        for required in ("plugin", "widget", "fallback"):
            if required not in widget:
                fail(f"plugin.widget requires {required}")
        identity(widget["plugin"], "plugin", NAME)
        identity(widget["widget"], "provider widget", NAME)
        fb = widget["fallback"]
        keys(fb, {"type"}, {"panel", "scene"}, "fallback")
        if fb["type"] not in {"empty", "theme.scene"}:
            fail("fallback must be empty or static scene")
        if fb["type"] == "theme.scene":
            if "scene" not in fb:
                fail("fallback theme.scene requires scene")
            identity(fb["scene"], "fallback scene", NAME)
        elif "scene" in fb:
            fail("empty fallback cannot include scene")


def validate_background(value):
    keys(value, {"mode"}, {"color", "asset", "fit", "paint", "focus"}, "background")
    mode = value["mode"]
    if mode == "none":
        keys(value, {"mode"}, (), "none background")
    elif mode == "solid":
        keys(value, {"mode", "color"}, (), "solid background")
        if value["color"] not in {"white", "black"}:
            fail("solid background color must be white or black")
    elif mode == "image":
        keys(value, {"mode", "asset"}, {"fit", "paint", "focus"}, "image background")
        identity(value["asset"], "background asset", NAME)
        if value.get("fit", "cover") not in {"cover", "contain", "native"}:
            fail("background.fit unsupported")
        if value.get("paint", "opaque") not in {"opaque", "ink-only"}:
            fail("background.paint unsupported")
        f = value.get("focus", [50, 50])
        if not isinstance(f, list) or len(f) != 2:
            fail("background focus must be two percentages")
        integer(f[0], 0, 100, "focus x")
        integer(f[1], 0, 100, "focus y")
    else:
        fail("background.mode unsupported")


def validate(doc, width=528, height=792, touch=False, inset=14):
    """Checks schema + minimum-fit geometry, not future real weighted allocation."""
    keys(doc, {"schema_version", "rows"}, {"background", "row_gap", "scene_underlays"}, "root")
    if type(doc["schema_version"]) is not int or doc["schema_version"] != 1:
        fail("unsupported schema version")
    validate_background(doc.get("background", {"mode": "none"}))
    row_gap = integer(doc.get("row_gap", 0), 0, 32, "row_gap")
    rows = doc["rows"]
    if not isinstance(rows, list) or not 1 <= len(rows) <= 8:
        fail("rows count out of bounds")
    canvas_height = height - (0 if touch else 40)
    canvas_width = width - 2 * inset
    if canvas_width <= 0:
        fail("canvas width too small")
    total_min_height = (len(rows) - 1) * row_gap
    row_ids = set()
    widget_count = 0
    for ri, row in enumerate(rows):
        keys(row, {"id", "weight", "min_height", "columns"},
             {"max_height", "column_gap", "size_mode"}, f"row {ri}")
        identity(row["id"], "row id")
        if row["id"] in row_ids:
            fail("duplicate row id")
        row_ids.add(row["id"])
        integer(row["weight"], 1, 10, "row weight")
        min_height = integer(row["min_height"], 1, height, "min_height")
        if "max_height" in row:
            integer(row["max_height"], min_height, height, "max_height")
        mode = row.get("size_mode", "weighted")
        if mode not in {"weighted", "fixed", "content"}:
            fail("size_mode unsupported")
        if mode == "fixed" and row.get("max_height") != min_height:
            fail("fixed row requires max_height == min_height")
        if mode == "content" and "max_height" not in row:
            fail("content row requires max_height")
        if ri == 0 and min_height < 40:
            fail("header row insufficient for native status")
        total_min_height += min_height
        gap = integer(row.get("column_gap", 0), 0, 32, "column_gap")
        cols = row["columns"]
        if not isinstance(cols, list) or not 1 <= len(cols) <= 4:
            fail("row columns count out of bounds")
        if ri == 0 and len(cols) != 1:
            fail("header must occupy its own row")
        if mode == "content" and (len(cols) != 1 or
                                  cols[0].get("widget", {}).get("type") != "native.shortcuts"):
            fail("content row V1 requires one native.shortcuts widget")
        widget_count += len(cols)
        seen = set()
        required_width = (len(cols) - 1) * gap
        for col in cols:
            keys(col, {"id", "weight", "min_width", "widget"},
                 {"max_width"}, f"row {ri} column")
            identity(col["id"], "column id")
            if col["id"] in seen:
                fail("duplicate column id")
            seen.add(col["id"])
            integer(col["weight"], 1, 10, "column weight")
            mw = integer(col["min_width"], 1, width, "min_width")
            if "max_width" in col:
                integer(col["max_width"], mw, width, "max_width")
            required_width += mw
            validate_widget(col["widget"], first_row=(ri == 0))
        if required_width > canvas_width:
            fail(f"row {ri}: minimum column widths exceed canvas")
    if widget_count > 16:
        fail("widget limit exceeded")
    if total_min_height > canvas_height:
        fail("minimum row heights exceed canvas")
    underlays = doc.get("scene_underlays", [])
    if not isinstance(underlays, list) or len(underlays) > 2:
        fail("scene_underlays must be a list of at most two entries")
    indices = {r["id"]: i for i, r in enumerate(rows)}
    used = set()
    for underlay in underlays:
        keys(underlay, {"scene", "from_row", "through_row"}, (), "scene underlay")
        identity(underlay["scene"], "scene underlay scene", NAME)
        start = indices.get(underlay["from_row"])
        end = indices.get(underlay["through_row"])
        if start is None or end is None or start == 0 or end >= len(rows) - 1 or end < start:
            fail("scene underlay must span valid forward rows below header and above footer/menu")
        for i in range(start, end + 1):
            if len(rows[i]["columns"]) != 1 or i in used:
                fail("scene underlay rows must be full-width and non-overlapping")
            if rows[i]["columns"][0]["widget"].get("type") == "native.shortcuts":
                fail("scene underlay must not cross the Home shortcuts menu")
            used.add(i)
    return {"rows": len(rows), "widgets": widget_count, "min_height": total_min_height}


class HomeLayoutContractTests(unittest.TestCase):
    def test_halo(self):
        obj = load_fixture("halo")
        self.assertEqual(validate(obj)["rows"], 4)
        self.assertEqual(validate(obj, 480, 800, touch=True)["widgets"], 4)

    def test_snoopy(self):
        obj = load_fixture("snoopy")
        self.assertEqual(validate(obj)["rows"], 5)
        self.assertEqual(validate(obj, 480, 800, touch=True)["widgets"], 5)

    def test_halo_scene_underlay_spans_hero_and_book(self):
        obj = load_fixture("halo")
        self.assertEqual(validate(obj)["widgets"], 4)
        self.assertEqual(obj["scene_underlays"][0]["from_row"], "hero")
        self.assertEqual(obj["scene_underlays"][0]["through_row"], "book")

    def test_scene_underlay_rejects_header_and_multicolumn(self):
        obj = load_fixture("halo")
        obj["scene_underlays"][0]["from_row"] = "header"
        with self.assertRaisesRegex(ContractError, "scene underlay"):
            validate(obj)
        obj = load_fixture("mixed")
        obj["scene_underlays"] = [
            {"scene": "mixed.hero", "from_row": "hero", "through_row": "book_stats"}
        ]
        with self.assertRaisesRegex(ContractError, "full-width"):
            validate(obj)

    def test_scene_underlay_rejects_reverse_overlap_and_unknown_keys(self):
        obj = load_fixture("halo")
        obj["scene_underlays"][0]["from_row"] = "book"
        obj["scene_underlays"][0]["through_row"] = "hero"
        with self.assertRaisesRegex(ContractError, "forward"):
            validate(obj)
        obj = load_fixture("halo")
        obj["scene_underlays"].append(dict(obj["scene_underlays"][0]))
        with self.assertRaisesRegex(ContractError, "non-overlapping"):
            validate(obj)
        obj = load_fixture("halo")
        obj["scene_underlays"][0]["execute"] = "not allowed"
        with self.assertRaisesRegex(ContractError, "unknown"):
            validate(obj)

    def test_underlay_cannot_cross_home_menu(self):
        obj = load_fixture("snoopy")
        obj["scene_underlays"] = [
            {"scene": "snoopy.hero", "from_row": "hero", "through_row": "menu"}
        ]
        with self.assertRaisesRegex(ContractError, "shortcuts menu"):
            validate(obj)

    def test_mixed_columns_and_background(self):
        obj = load_fixture("mixed")
        self.assertEqual([len(r["columns"]) for r in obj["rows"]], [1, 1, 2, 3, 1])
        self.assertEqual(validate(obj)["widgets"], 8)
        self.assertEqual(validate(obj, 480, 800, touch=True)["rows"], 5)

    def test_optional_missing_provider_with_fallback(self):
        obj = load_fixture("missing_plugin")
        self.assertEqual(validate(obj)["widgets"], 3)

    def test_impossible_height_rejected(self):
        with self.assertRaisesRegex(ContractError, "minimum row heights"):
            validate(load_fixture("invalid_minima"))

    def test_duplicate_row_id_rejected(self):
        obj = load_fixture("halo")
        obj["rows"][1]["id"] = obj["rows"][0]["id"]
        with self.assertRaisesRegex(ContractError, "duplicate row"):
            validate(obj)

    def test_too_many_rows(self):
        obj = load_fixture("mixed")
        obj["rows"].extend(copy.deepcopy(obj["rows"][1:5]))
        with self.assertRaisesRegex(ContractError, "rows count"):
            validate(obj)

    def test_row_column_split_limit(self):
        obj = load_fixture("mixed")
        row = obj["rows"][3]
        row["columns"].append(copy.deepcopy(row["columns"][0]))
        row["columns"].append(copy.deepcopy(row["columns"][1]))
        with self.assertRaisesRegex(ContractError, "row columns count"):
            validate(obj)

    def test_invalid_weight(self):
        obj = load_fixture("halo")
        obj["rows"][1]["weight"] = True
        with self.assertRaisesRegex(ContractError, "row weight"):
            validate(obj)

    def test_content_row_requires_max_and_shortcuts(self):
        obj = load_fixture("halo")
        del obj["rows"][3]["max_height"]
        with self.assertRaisesRegex(ContractError, "content row requires max_height"):
            validate(obj)
        obj = load_fixture("halo")
        obj["rows"][3]["columns"][0]["widget"] = {"type": "native.book"}
        with self.assertRaisesRegex(ContractError, "content row V1 requires"):
            validate(obj)

    def test_fixed_row_requires_matching_bounds(self):
        obj = load_fixture("snoopy")
        obj["rows"][4]["size_mode"] = "fixed"
        self.assertEqual(validate(obj)["rows"], 5)
        obj["rows"][4]["max_height"] = 80
        with self.assertRaisesRegex(ContractError, "fixed row requires"):
            validate(obj)

    def test_unknown_widget_not_accepted(self):
        obj = load_fixture("halo")
        obj["rows"][2]["columns"][0]["widget"]["type"] = "execute.lua"
        with self.assertRaisesRegex(ContractError, "unsupported type"):
            validate(obj)

    def test_missing_plugin_fallback_required(self):
        obj = load_fixture("missing_plugin")
        del obj["rows"][1]["columns"][0]["widget"]["fallback"]
        with self.assertRaisesRegex(ContractError, "requires fallback"):
            validate(obj)

    def test_invalid_background_focal_point(self):
        obj = load_fixture("mixed")
        obj["background"]["focus"] = [120, 0]
        with self.assertRaisesRegex(ContractError, "focus x"):
            validate(obj)

    def test_unknown_key_not_accepted(self):
        obj = load_fixture("halo")
        obj["rows"][1]["javascript"] = "alert('hi')"
        with self.assertRaisesRegex(ContractError, "unknown"):
            validate(obj)

    def test_header_cannot_share_row(self):
        obj = load_fixture("mixed")
        obj["rows"][0]["columns"].append(copy.deepcopy(obj["rows"][2]["columns"][0]))
        with self.assertRaisesRegex(ContractError, "header"):
            validate(obj)

    def test_duplicate_json_keys_not_accepted(self):
        with self.assertRaisesRegex(ContractError, "duplicate JSON key"):
            json.loads('{"rows":[],"rows":[]}', object_pairs_hook=parse_unique_pairs)


if __name__ == "__main__":
    unittest.main()
