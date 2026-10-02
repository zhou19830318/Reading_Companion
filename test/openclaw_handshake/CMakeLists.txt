cmake_minimum_required(VERSION 3.16)

# The vendored ed25519 sources are C; the rest of the tree is C++.
set(ED25519_SOURCES
  ${REPO_ROOT}/lib/ed25519/keypair.c
  ${REPO_ROOT}/lib/ed25519/sign.c
  ${REPO_ROOT}/lib/ed25519/verify.c
  ${REPO_ROOT}/lib/ed25519/fe.c
  ${REPO_ROOT}/lib/ed25519/ge.c
  ${REPO_ROOT}/lib/ed25519/sc.c
  ${REPO_ROOT}/lib/ed25519/sha512.c
)

add_executable(OpenClawHandshakeTest
  OpenClawHandshakeTest.cpp
  ${REPO_ROOT}/lib/OpenClaw/OpenClawHandshake.cpp
  ${REPO_ROOT}/lib/JsonParser/StreamingJsonParser.cpp
  ${ED25519_SOURCES}
)

# Third-party C: keep our warning set from failing the build on upstream style.
set_source_files_properties(${ED25519_SOURCES} PROPERTIES
  COMPILE_OPTIONS "-Wall;-Wno-sign-compare;-Wno-unused-parameter"
)

target_include_directories(OpenClawHandshakeTest PRIVATE
  ${REPO_ROOT}/lib/OpenClaw
  ${REPO_ROOT}/lib/JsonParser
  ${REPO_ROOT}/lib/ed25519
)

target_link_libraries(OpenClawHandshakeTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(OpenClawHandshakeTest)
