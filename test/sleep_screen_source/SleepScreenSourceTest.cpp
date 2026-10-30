#include <gtest/gtest.h>

#include "util/SleepScreenSource.h"

namespace {
using Mode = CrossPointSettings::SLEEP_SCREEN_MODE;

constexpr Mode kModesWithoutCustomSource[] = {
    Mode::DARK,
    Mode::LIGHT,
    Mode::COVER,
    Mode::BLANK,
    Mode::QUICK_RESUME,
    Mode::TRANSPARENT_CUSTOM,
    Mode::COVER_TRANSPARENT,
};
}  // namespace

TEST(SleepScreenSource, CustomAndCoverCustomUseCustomSource) {
  EXPECT_TRUE(sleepscreen::usesCustomSource(Mode::CUSTOM));
  EXPECT_TRUE(sleepscreen::usesCustomSource(Mode::COVER_CUSTOM));
}

TEST(SleepScreenSource, OtherModesDoNotUseCustomSource) {
  for (const Mode mode : kModesWithoutCustomSource) {
    EXPECT_FALSE(sleepscreen::usesCustomSource(mode)) << "mode=" << static_cast<int>(mode);
  }
}

TEST(SleepScreenSource, CustomAlwaysShowsCustomSource) {
  EXPECT_TRUE(sleepscreen::showsCustomSource(Mode::CUSTOM, false));
  EXPECT_TRUE(sleepscreen::showsCustomSource(Mode::CUSTOM, true));
}

TEST(SleepScreenSource, CoverCustomShowsCustomSourceOnlyOutsideReader) {
  EXPECT_TRUE(sleepscreen::showsCustomSource(Mode::COVER_CUSTOM, false));
  EXPECT_FALSE(sleepscreen::showsCustomSource(Mode::COVER_CUSTOM, true));
}

TEST(SleepScreenSource, OtherModesNeverShowCustomSource) {
  for (const Mode mode : kModesWithoutCustomSource) {
    EXPECT_FALSE(sleepscreen::showsCustomSource(mode, false)) << "mode=" << static_cast<int>(mode);
    EXPECT_FALSE(sleepscreen::showsCustomSource(mode, true)) << "mode=" << static_cast<int>(mode);
  }
}
