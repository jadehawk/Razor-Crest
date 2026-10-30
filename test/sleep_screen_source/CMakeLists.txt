add_executable(SleepScreenSourceTest SleepScreenSourceTest.cpp)
target_include_directories(SleepScreenSourceTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
  ${REPO_ROOT}/src
)
target_link_libraries(SleepScreenSourceTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(SleepScreenSourceTest)
