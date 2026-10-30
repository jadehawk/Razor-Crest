#include <gtest/gtest.h>

#include "network/OtaVersion.h"

TEST(OtaVersion, ParsesReleaseTag) {
  const auto v = otaversion::parse("razor-crest-v2.0.0-r5");
  EXPECT_TRUE(v.valid);
  EXPECT_EQ(v.major, 2);
  EXPECT_EQ(v.minor, 0);
  EXPECT_EQ(v.patch, 0);
  EXPECT_EQ(v.revision, 5);
  EXPECT_FALSE(v.releaseCandidate);
  EXPECT_FALSE(v.development);
}

TEST(OtaVersion, ParsesCompiledVersion) {
  const auto v = otaversion::parse("2.0.0-r5");
  EXPECT_TRUE(v.valid);
  EXPECT_EQ(v.revision, 5);
}

TEST(OtaVersion, ParsesVPrefixAndMultiDigitRevision) {
  const auto v = otaversion::parse("v2.0.0-r12");
  EXPECT_TRUE(v.valid);
  EXPECT_EQ(v.revision, 12);
}

TEST(OtaVersion, ParsesCandidateAndDevelopmentBuilds) {
  EXPECT_TRUE(otaversion::parse("2.0.0-rc1").releaseCandidate);
  EXPECT_TRUE(otaversion::parse("2.0.0-dev-razor-crest-abc1234").development);
}

TEST(OtaVersion, ParsesBareCandidateSuffix) {
  EXPECT_TRUE(otaversion::parse("2.0.0rc").releaseCandidate);
  EXPECT_TRUE(otaversion::parse("v2.0.0-rc1").releaseCandidate);
}

TEST(OtaVersion, BuildsUpstreamAssetName) {
  char out[64] = {};
  ASSERT_TRUE(otaversion::buildAssetName("crosspoint-", "2.0.0rc", "-x4pro", out, sizeof(out)));
  EXPECT_STREQ(out, "crosspoint-2.0.0rc-x4pro.bin");
}

TEST(OtaVersion, RejectsGarbage) {
  EXPECT_FALSE(otaversion::parse("not-a-version").valid);
  EXPECT_FALSE(otaversion::parse("").valid);
  EXPECT_FALSE(otaversion::parse(nullptr).valid);
}

TEST(OtaVersion, SameRevisionIsNotNewer) {
  EXPECT_FALSE(otaversion::isNewer("razor-crest-v2.0.0-r5", "2.0.0-r5-x4pro"));
}

TEST(OtaVersion, HigherRevisionIsNewer) { EXPECT_TRUE(otaversion::isNewer("razor-crest-v2.0.0-r6", "2.0.0-r5-x4pro")); }

TEST(OtaVersion, LowerRevisionIsNotNewer) {
  EXPECT_FALSE(otaversion::isNewer("razor-crest-v2.0.0-r4", "2.0.0-r5-x4pro"));
}

TEST(OtaVersion, HigherBaseVersionWinsOverRevision) {
  EXPECT_TRUE(otaversion::isNewer("razor-crest-v2.0.1-r1", "2.0.0-r9-x4pro"));
  EXPECT_FALSE(otaversion::isNewer("razor-crest-v1.6.5-r99", "2.0.0-r1-x4pro"));
}

TEST(OtaVersion, UpstreamCandidateIsOlderThanFinalRelease) {
  EXPECT_TRUE(otaversion::isNewer("2.0.0", "2.0.0-rc+abc123"));
  EXPECT_FALSE(otaversion::isNewer("1.6.5", "2.0.0"));
}

TEST(OtaVersion, BuildsUpstreamBoardAssetName) {
  char out[64] = {};
  ASSERT_TRUE(otaversion::buildAssetName("crosspoint-", "2.0.0rc", "-metalio_eink4", out, sizeof(out)));
  EXPECT_STREQ(out, "crosspoint-2.0.0rc-metalio_eink4.bin");
}

TEST(OtaVersion, DevelopmentBuildIsNeverDowngraded) {
  EXPECT_FALSE(otaversion::isNewer("razor-crest-v2.0.0-r5", "2.0.0-dev-razor-crest-abc1234"));
}

TEST(OtaVersion, ReleaseSupersedesCandidate) {
  EXPECT_TRUE(otaversion::isNewer("razor-crest-v2.0.0-r1", "2.0.0-rc1-x4pro"));
}

TEST(OtaVersion, BuildsReleaseAssetName) {
  char out[64] = {};
  ASSERT_TRUE(otaversion::buildAssetName("razor-crest-", "razor-crest-v2.0.0-r5", "-x4pro", out, sizeof(out)));
  EXPECT_STREQ(out, "razor-crest-2.0.0-r5-x4pro.bin");

  ASSERT_TRUE(otaversion::buildAssetName("razor-crest-", "razor-crest-v2.0.0-r5", "-x3-x4", out, sizeof(out)));
  EXPECT_STREQ(out, "razor-crest-2.0.0-r5-x3-x4.bin");

  ASSERT_TRUE(otaversion::buildAssetName("razor-crest-", "razor-crest-v2.0.0-r5", "-metalio-eink4", out, sizeof(out)));
  EXPECT_STREQ(out, "razor-crest-2.0.0-r5-metalio-eink4.bin");

  ASSERT_TRUE(otaversion::buildAssetName("razor-crest-", "razor-crest-v2.0.0-r5", "-eego-a4", out, sizeof(out)));
  EXPECT_STREQ(out, "razor-crest-2.0.0-r5-eego-a4.bin");
}

TEST(OtaVersion, RejectsAssetNameThatDoesNotFit) {
  char out[16] = {};
  EXPECT_FALSE(otaversion::buildAssetName("razor-crest-", "razor-crest-v2.0.0-r5", "-x4pro", out, sizeof(out)));
}

// The actual firmware tag is "CROSSPOINT-BOARD-V1:<board>;", so boardName()
// points at a non-null-terminated substring. These tests include the semicolon.
TEST(OtaVersion, LengthDelimitedBoardNamesSelectSpecialAssets) {
  char suffix[24] = {};
  ASSERT_TRUE(otaversion::buildBoardAssetSuffix("x4;", 2, true, suffix, sizeof(suffix)));
  EXPECT_STREQ(suffix, "-x3-x4");

  ASSERT_TRUE(
      otaversion::buildBoardAssetSuffix("metalio_eink4;", sizeof("metalio_eink4") - 1, true, suffix, sizeof(suffix)));
  EXPECT_STREQ(suffix, "-metalio-eink4");

  ASSERT_TRUE(otaversion::buildBoardAssetSuffix("eego_a4;", sizeof("eego_a4") - 1, true, suffix, sizeof(suffix)));
  EXPECT_STREQ(suffix, "-eego-a4");
}

TEST(OtaVersion, UpstreamBoardSuffixesKeepUnderscores) {
  char suffix[24] = {};
  ASSERT_TRUE(otaversion::buildBoardAssetSuffix("x4;", 2, false, suffix, sizeof(suffix)));
  EXPECT_STREQ(suffix, "-x3-x4");

  ASSERT_TRUE(
      otaversion::buildBoardAssetSuffix("metalio_eink4;", sizeof("metalio_eink4") - 1, false, suffix, sizeof(suffix)));
  EXPECT_STREQ(suffix, "-metalio_eink4");

  ASSERT_TRUE(otaversion::buildBoardAssetSuffix("eego_a4;", sizeof("eego_a4") - 1, false, suffix, sizeof(suffix)));
  EXPECT_STREQ(suffix, "-eego_a4");
}

TEST(OtaVersion, AllPublishedRazorCrestBoardsBuildActualReleaseAssetNames) {
  struct BoardCase {
    const char* taggedBoard;
    size_t boardLen;
    const char* asset;
  };
  const BoardCase boards[] = {
      {"x4;", 2, "razor-crest-2.0.0-r6-x3-x4.bin"},
      {"x4pro;", sizeof("x4pro") - 1, "razor-crest-2.0.0-r6-x4pro.bin"},
      {"x4c;", sizeof("x4c") - 1, "razor-crest-2.0.0-r6-x4c.bin"},
      {"papermono;", sizeof("papermono") - 1, "razor-crest-2.0.0-r6-papermono.bin"},
      {"sticky;", sizeof("sticky") - 1, "razor-crest-2.0.0-r6-sticky.bin"},
      {"metalio_eink4;", sizeof("metalio_eink4") - 1, "razor-crest-2.0.0-r6-metalio-eink4.bin"},
      {"eego_a4;", sizeof("eego_a4") - 1, "razor-crest-2.0.0-r6-eego-a4.bin"},
  };
  for (const auto& board : boards) {
    char suffix[24] = {};
    char asset[48] = {};
    ASSERT_TRUE(otaversion::buildBoardAssetSuffix(board.taggedBoard, board.boardLen, true, suffix, sizeof(suffix)))
        << board.taggedBoard;
    ASSERT_TRUE(otaversion::buildAssetName("razor-crest-", "razor-crest-v2.0.0-r6", suffix, asset, sizeof(asset)));
    EXPECT_STREQ(asset, board.asset) << board.taggedBoard;
  }
}

TEST(OtaVersion, BoardSuffixRejectsInvalidInputAndTruncation) {
  char suffix[8] = {};
  EXPECT_FALSE(otaversion::buildBoardAssetSuffix(nullptr, 2, true, suffix, sizeof(suffix)));
  EXPECT_FALSE(otaversion::buildBoardAssetSuffix("x4;", 0, true, suffix, sizeof(suffix)));
  EXPECT_FALSE(otaversion::buildBoardAssetSuffix("x4;", 2, true, nullptr, sizeof(suffix)));
  EXPECT_FALSE(
      otaversion::buildBoardAssetSuffix("metalio_eink4;", sizeof("metalio_eink4") - 1, true, suffix, sizeof(suffix)));
  EXPECT_FALSE(otaversion::buildBoardAssetSuffix("x4pro;", sizeof("x4pro") - 1, true, suffix, 0));
}
