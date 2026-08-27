add_executable(PhoneSyncProtocolTest
  PhoneSyncProtocolTest.cpp
  ${REPO_ROOT}/lib/PhoneSync/PhoneSyncProtocol.cpp
)

target_include_directories(PhoneSyncProtocolTest PRIVATE
  ${REPO_ROOT}/lib/PhoneSync
)

target_link_libraries(PhoneSyncProtocolTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(PhoneSyncProtocolTest)
