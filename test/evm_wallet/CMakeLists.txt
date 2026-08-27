add_executable(EvmWalletTest
  EvmWalletTest.cpp
  ${REPO_ROOT}/lib/EvmWallet/EvmWalletProtocol.cpp
  ${REPO_ROOT}/lib/EvmWallet/EvmSignable.cpp
  ${REPO_ROOT}/lib/EvmWallet/EvmTransaction.cpp
  ${REPO_ROOT}/lib/EvmWallet/Keccak256.cpp
)

target_include_directories(EvmWalletTest PRIVATE
  ${REPO_ROOT}/lib/EvmWallet
)

target_link_libraries(EvmWalletTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(EvmWalletTest)
