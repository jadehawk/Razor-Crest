add_executable(OtaVersionTest
  OtaVersionTest.cpp
  ${REPO_ROOT}/src/network/OtaVersion.cpp
)
target_include_directories(OtaVersionTest PRIVATE
  ${REPO_ROOT}/src
)
target_link_libraries(OtaVersionTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(OtaVersionTest)
