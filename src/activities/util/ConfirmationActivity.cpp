#include "ConfirmationActivity.h"

#include <I18n.h>

#include "HalDisplay.h"
#include "components/UITheme.h"

ConfirmationActivity::ConfirmationActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                           const std::string& heading, const std::string& body,
                                           const std::string& cancelLabel, const std::string& confirmLabel,
                                           bool useMessageLayout)
    : Activity("Confirmation", renderer, mappedInput),
      heading(heading),
      body(body),
      cancelLabel(cancelLabel),
      confirmLabel(confirmLabel),
      useMessageLayout(useMessageLayout) {}

void ConfirmationActivity::onEnter() {
  Activity::onEnter();

  // Ordinary confirmations wrap their subject as a short headline. The
  // battery warning uses the longer message layout without changing them.
  const char* options[] = {
      cancelLabel.empty() ? I18N.get(StrId::STR_CANCEL) : cancelLabel.c_str(),
      confirmLabel.empty() ? I18N.get(StrId::STR_CONFIRM) : confirmLabel.c_str(),
  };
  auto onSelect = [this](int idx) {
    ActivityResult res;
    res.isCancelled = (idx != 1);
    setResult(std::move(res));
    finish();
  };
  if (useMessageLayout) {
    confirmPopup.showMessage(heading.c_str(), body.c_str(), options, 2, 0, std::move(onSelect));
  } else {
    confirmPopup.show(heading.c_str(), body.c_str(), options, 2, 0, std::move(onSelect));
  }

  requestUpdate(true);
}

void ConfirmationActivity::render(RenderLock&& lock) {
  renderer.clearScreen();

  if (confirmPopup.processRender(renderer, mappedInput)) return;

  renderer.displayBuffer(HalDisplay::RefreshMode::FAST_REFRESH);
}

void ConfirmationActivity::loop() {
  if (confirmPopup.handleInput(mappedInput, [this] {
        if (confirmPopup.isActive()) requestUpdate();
      }))
    return;

  // Popup dismissed without a selection (Back button or tap outside): cancel.
  ActivityResult res;
  res.isCancelled = true;
  setResult(std::move(res));
  finish();
}
