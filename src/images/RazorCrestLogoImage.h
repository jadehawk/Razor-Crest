#pragma once

#include <cstdint>

#include "images/Logo120.h"
#include "images/RazorCrestBootLogo.h"

struct RazorCrestLogoImage {
  const uint8_t* pixels;
  int size;
};

// The X3's 528px portrait width fits the full 500px art. The X4Pro's
// 480px portrait panel uses 460px; smaller panels retain the stock logo.
inline RazorCrestLogoImage selectRazorCrestLogoImage(int width, int height) {
  if (width >= 500 && height >= 720) return {RAZOR_CREST_BOOT_LOGO_500, 500};
  if (width >= 460 && height >= 700) return {RAZOR_CREST_BOOT_LOGO_460, 460};
  return {Logo120, 200};
}
