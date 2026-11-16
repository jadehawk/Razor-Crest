#pragma once

// Device-native Home widget paint bindings. The compositor uses these hooks
// without owning a framebuffer. Callbacks for header/cover/shortcuts must be
// supplied by HomeActivity using the EXISTING GUI/ThemeScene renderers.
#include <algorithm>
#include <cstdint>
#include <string>

#include <GfxRenderer.h>

#include "components/home/HomeWidgetEngine.h"
#include "components/themes/CpbBitmap.h"

namespace home_widgets::native {
struct Surface {
  const GfxRenderer& renderer;
  void* owner = nullptr;
  const std::string* (*resolveBitmap)(void*, uint16_t asset) = nullptr;
  bool (*drawScene)(void*, uint16_t scene, Rect area) = nullptr;
  bool (*drawNative)(void*, Kind kind, uint16_t resource, Rect area, Panel panel) = nullptr;
  bool (*isProviderReady)(void*, uint16_t provider) = nullptr;
  int savedClip[2][4]{};
  uint8_t clipDepth = 0;
};
static_assert(sizeof(Surface) <= 128, "Home paint surface exceeds bounded V1 size");

inline void pushClip(void* context, Rect rect) {
  auto& s = *static_cast<Surface*>(context);
  const auto old = s.renderer.getClipRect();
  if (s.clipDepth >= 2) return;  // engine never nests clips; defensive.
  for (int i = 0; i < 4; ++i) s.savedClip[s.clipDepth][i] = old[i];
  ++s.clipDepth;
  const int x = std::max(old[0], static_cast<int>(rect.x));
  const int y = std::max(old[1], static_cast<int>(rect.y));
  const int right = std::min(old[0] + old[2], static_cast<int>(rect.x) + rect.width);
  const int bottom = std::min(old[1] + old[3], static_cast<int>(rect.y) + rect.height);
  s.renderer.setClipRect(x, y, std::max(0, right - x), std::max(0, bottom - y));
}

inline void popClip(void* context) {
  auto& s = *static_cast<Surface*>(context);
  if (!s.clipDepth) return;
  const int* old = s.savedClip[--s.clipDepth];
  s.renderer.setClipRect(old[0], old[1], old[2], old[3]);
}

inline void fill(void* context, Rect rect, bool black) {
  const auto& renderer = static_cast<Surface*>(context)->renderer;
  renderer.fillRect(rect.x, rect.y, rect.width, rect.height, black);
}

inline bool bitmap(void* context, const Background& background, Rect canvas) {
  const auto& s = *static_cast<Surface*>(context);
  if (!s.resolveBitmap || background.bitmap == 0) return false;
  const std::string* path = s.resolveBitmap(s.owner, background.bitmap);
  if (!path || path->empty()) return false;
  themebitmap::Options options;
  options.x = canvas.x;
  options.y = canvas.y;
  options.width = canvas.width;
  options.height = canvas.height;
  options.fit = background.fit == ImageFit::Contain ? themebitmap::Fit::Contain
                : background.fit == ImageFit::Native ? themebitmap::Fit::Native
                                                     : themebitmap::Fit::Cover;
  options.inkOnly = background.inkOnly;
  options.inkBlack = true;
  // Existing themebitmap::draw validates the CPB CRC before painting. It
  // streams 128-byte scanlines and does not allocate a second framebuffer.
  // Theme Studio bakes the selected focal-point crop into the device CPB.
  return themebitmap::draw(s.renderer, *path, options);
}

inline bool scene(void* context, uint16_t sceneId, Rect area) {
  const auto& s = *static_cast<Surface*>(context);
  return s.drawScene && s.drawScene(s.owner, sceneId, area);
}

inline bool nativeWidget(void* context, Kind kind, uint16_t resource, Rect area, Panel panel) {
  const auto& s = *static_cast<Surface*>(context);
  return s.drawNative && s.drawNative(s.owner, kind, resource, area, panel);
}

inline bool providerReady(void* context, uint16_t provider) {
  const auto& s = *static_cast<Surface*>(context);
  return s.isProviderReady && s.isProviderReady(s.owner, provider);
}

inline PaintHooks hooks(Surface& surface) {
  PaintHooks out;
  out.user = &surface;
  out.pushClip = pushClip;
  out.popClip = popClip;
  out.fill = fill;
  out.bitmap = bitmap;
  out.scene = scene;
  out.native = nativeWidget;
  out.providerAvailable = providerReady;
  return out;
}
}  // namespace home_widgets::native
