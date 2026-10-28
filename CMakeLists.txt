cmake_minimum_required(VERSION 3.16)
project(svxlink-benshi VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(GNUInstallDirs)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

find_package(Threads REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(BLUEZ REQUIRED bluez)

add_compile_options(-Wall -Wextra -Wpedantic)

# ---------------------------------------------------------------------------
# Google libsbc (Apache-2.0). This is the same implementation used by
# BenshiNode, which keeps the radio's SBC stream bit-for-bit compatible.
# ---------------------------------------------------------------------------
add_library(sbc STATIC
  third_party/sbc/sbc.c
  third_party/sbc/bits.c
)
target_include_directories(sbc SYSTEM
  PUBLIC  third_party/sbc/include
  PRIVATE third_party/sbc
)
target_compile_options(sbc PRIVATE -w)
set_target_properties(sbc PROPERTIES POSITION_INDEPENDENT_CODE ON)

# ---------------------------------------------------------------------------
# Core bridge
# ---------------------------------------------------------------------------
add_library(benshi_core STATIC
  src/Bridge.cpp
  src/Config.cpp
  src/Logger.cpp
  src/audio/DtmfDecoder.cpp
  src/audio/Resampler.cpp
  src/bluetooth/RfcommChannel.cpp
  src/net/NetTrxServer.cpp
  src/radio/AudioFraming.cpp
  src/radio/GaiaProtocol.cpp
  src/radio/RadioSession.cpp
  src/radio/SbcCodec.cpp
  src/radio/Vr76Audio.cpp
)
target_include_directories(benshi_core
  PUBLIC  src
  PRIVATE ${BLUEZ_INCLUDE_DIRS}
)
target_link_directories(benshi_core PRIVATE ${BLUEZ_LIBRARY_DIRS})
target_link_libraries(benshi_core
  PUBLIC  sbc Threads::Threads OpenSSL::Crypto
  PRIVATE ${BLUEZ_LIBRARIES}
)

add_executable(svxlink-benshi src/main.cpp)
target_link_libraries(svxlink-benshi PRIVATE benshi_core)
target_compile_definitions(svxlink-benshi PRIVATE
  SVXLINK_BENSHI_VERSION="${PROJECT_VERSION}"
)

install(TARGETS svxlink-benshi
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
install(FILES config/svxlink-benshi.conf
  DESTINATION ${CMAKE_INSTALL_SYSCONFDIR}
)
configure_file(
  systemd/svxlink-benshi.service.in
  ${CMAKE_CURRENT_BINARY_DIR}/svxlink-benshi.service
  @ONLY
)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/svxlink-benshi.service
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/systemd/system
)
install(FILES README.md INSTALL.md CHANGELOG.md LICENSE
  config/svxlink.conf.example systemd/svxlink.service.example
  DESTINATION ${CMAKE_INSTALL_DOCDIR}
)

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
enable_testing()
add_executable(benshi_tests tests/core_tests.cpp)
target_link_libraries(benshi_tests PRIVATE benshi_core)
add_test(NAME benshi_tests COMMAND benshi_tests)
