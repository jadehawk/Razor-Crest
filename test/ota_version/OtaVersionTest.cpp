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
  const auto v = otaversion::parse("2.0.0-r5-x4pro");
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
