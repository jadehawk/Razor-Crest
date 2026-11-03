#include "BootActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>

#include "components/UITheme.h"
#include "fontIds.h"
#include "images/RazorCrestLogoImage.h"
#include "util/FirmwareIdentity.h"

void BootActivity::onEnter() {
  Activity::onEnter();

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();
  if (UITheme::getInstance().renderExternalScene(ThemeScene::Region::Boot, renderer,
                                                 Rect{0, 0, pageWidth, pageHeight})) {
    renderer.displayBuffer();
    return;
  }

  // Keep the built-in boot and sleep screens on the same pre-rendered art.
  const auto logo = selectRazorCrestLogoImage(pageWidth, pageHeight);
  const int logoSize = logo.size;

  const int taglineHeight = renderer.getLineHeight(UI_12_FONT_ID);
  const int brandHeight = renderer.getLineHeight(UI_10_FONT_ID);
  const int bootHeight = renderer.getLineHeight(SMALL_FONT_ID);
  constexpr int artGap = 16;
  constexpr int brandGap = 28;
  constexpr int bootGap = 6;
  constexpr int footerReserve = 54;
  const int blockHeight = logoSize + artGap + taglineHeight + brandGap + brandHeight + bootGap + bootHeight;
  const int top = std::max(8, (pageHeight - footerReserve - blockHeight) / 2);

  renderer.drawImage(logo.pixels, (pageWidth - logoSize) / 2, top, logoSize, logoSize);
  const int taglineY = top + logoSize + artGap;
  renderer.drawCenteredText(UI_12_FONT_ID, taglineY, "This Is the Way", true, EpdFontFamily::BOLD);
  const int brandY = taglineY + taglineHeight + brandGap;
  renderer.drawCenteredText(UI_10_FONT_ID, brandY, firmwareidentity::ID, true, EpdFontFamily::BOLD);
  renderer.drawCenteredText(SMALL_FONT_ID, brandY + brandHeight + bootGap, tr(STR_BOOTING));
  renderer.drawCenteredText(SMALL_FONT_ID, pageHeight - 30, CROSSPOINT_VERSION);
  renderer.displayBuffer();
}
