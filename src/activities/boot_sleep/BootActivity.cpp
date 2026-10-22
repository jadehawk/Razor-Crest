#include "BootActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include "fontIds.h"
#include "components/UITheme.h"
#include "images/Logo120.h"
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
  renderer.clearScreen();
  renderer.drawImage(Logo120, (pageWidth - 200) / 2, (pageHeight - 200) / 2, 200, 200);
  renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 + 110, firmwareidentity::ID, true, EpdFontFamily::BOLD);
  renderer.drawCenteredText(SMALL_FONT_ID, pageHeight / 2 + 135, tr(STR_BOOTING));
  renderer.drawCenteredText(SMALL_FONT_ID, pageHeight - 30, CROSSPOINT_VERSION);
  renderer.displayBuffer();
}
