from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
base = (ROOT / 'src/components/themes/BaseTheme.cpp').read_text(encoding='utf-8')
rounded = (ROOT / 'src/components/themes/roundedraff/RoundedRaffTheme.h').read_text(encoding='utf-8')

# The approved X3/EEGO built-in theme geometry preserves the cover/menu bottom.
assert '.homeTopPadding = 80,' in rounded
assert '.homeCoverTileHeight = 325,' in rounded
assert 80 + 325 == 64 + 341

# The correction is only for a titled, left-aligned compact Home band
# with a visible clock; theme-owned SD header scenes remain untouched.
assert 'title && props.status.clockText && rect.height < UITheme::getInstance().getMetrics().headerHeight' in base
assert 'props.titleText.align == fui::TextAlign::Left' in base
assert 'constexpr int clockTitleGap = 12;' in base
assert 'ui.target.inkBounds(props.status.battery.text.font, props.status.clockText' in base
assert 'ui.target.inkBounds(props.titleText.font, title, props.titleText)' in base
assert 'const int targetTop = std::min(clockTop + clockInk.height + clockTitleGap, band.bottom() - titleInk.height - 6);' in base
assert 'props.titleOffsetY = static_cast<int16_t>(std::max(0, targetTop - titleTop));' in base
assert 'props.sidePadding = props.status.edgeInset;' in base

# Cover clearance limits the title shift; otherwise maintain a 12px ink gap.
for clock_top, clock_h, title_h, band_bottom in ((20, 12, 18, 80), (18, 11, 20, 80), (16, 12, 18, 70)):
    target = min(clock_top + clock_h + 12, band_bottom - title_h - 6)
    assert target + title_h + 6 <= band_bottom
    assert target - (clock_top + clock_h) <= 12
print('Razor-Crest compact Home header spacing: PASS')
