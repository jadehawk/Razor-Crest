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
  for (const bool fromReader : {false, true}) {
    for (const bool coverFallback : {false, true}) {
      EXPECT_TRUE(sleepscreen::showsCustomSource(Mode::CUSTOM, fromReader, coverFallback));
    }
  }
}

TEST(SleepScreenSource, CoverCustomFromHomeAlwaysShowsCustomSource) {
  EXPECT_TRUE(sleepscreen::showsCustomSource(Mode::COVER_CUSTOM, false, false));
  EXPECT_TRUE(sleepscreen::showsCustomSource(Mode::COVER_CUSTOM, false, true));
}

TEST(SleepScreenSource, CoverCustomFromReaderPreservesValidCover) {
  EXPECT_FALSE(sleepscreen::showsCustomSource(Mode::COVER_CUSTOM, true, false));
}

TEST(SleepScreenSource, CoverCustomFromReaderRefreshesMissingCoverFallback) {
  EXPECT_TRUE(sleepscreen::showsCustomSource(Mode::COVER_CUSTOM, true, true));
}

TEST(SleepScreenSource, OtherModesNeverShowCustomSource) {
  for (const Mode mode : kModesWithoutCustomSource) {
    for (const bool fromReader : {false, true}) {
      for (const bool coverFallback : {false, true}) {
        EXPECT_FALSE(sleepscreen::showsCustomSource(mode, fromReader, coverFallback))
            << "mode=" << static_cast<int>(mode) << " fromReader=" << fromReader
            << " coverFallback=" << coverFallback;
      }
    }
  }
}
