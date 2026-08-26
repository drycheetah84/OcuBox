cmake_minimum_required(VERSION 3.21)
project(hollywood_emu LANGUAGES CXX VERSION 0.1.0)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if (MSVC)
  add_compile_options(/W4 /permissive- /Zc:__cplusplus /utf-8 /EHsc)
  add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
endif()

# --- Third-party (via bundled vcpkg manifest mode) ---
find_package(LibLZMA REQUIRED)              # XZ / LZMA  -> LibLZMA::LibLZMA
find_package(BZip2   REQUIRED)              # bzip2      -> BZip2::BZip2
find_package(ZLIB    REQUIRED)              # gzip/zlib  -> ZLIB::ZLIB
find_package(capstone CONFIG REQUIRED)     # disasm     -> capstone::capstone

# We build Unicorn from source ourselves (third_party/) with a patch that
# exposes SPSR_EL1 -- required to correctly emulate ARM64 exception entry/return,
# which the prebuilt vcpkg binary cannot do. Static-link the patched archive.
set(UNICORN_INCLUDE_DIR "${CMAKE_SOURCE_DIR}/third_party/unicorn-2.1.4/include")
set(UNICORN_LIBRARY "${CMAKE_SOURCE_DIR}/third_party/unicorn-build/Release/unicorn.lib")
if(NOT EXISTS "${UNICORN_LIBRARY}")
  message(FATAL_ERROR "patched unicorn.lib not found -- build third_party/unicorn-build first")
endif()
message(STATUS "Unicorn (patched, static): ${UNICORN_LIBRARY}")

# --- hollywood_emu core library (all real subsystems) ---
add_library(hwemu_core STATIC
  src/common/log.cpp
  src/common/compress.cpp
  src/ota/zip_reader.cpp
  src/ota/payload.cpp
  src/boot/boot_image.cpp
  src/boot/dtb.cpp
  src/memory/guest_memory.cpp
  src/devices/device_bus.cpp
  src/devices/geni_uart.cpp
  src/devices/gic.cpp
  src/devices/qcom_rsc.cpp
  src/devices/qcom_gcc.cpp
  src/devices/qcom_ufs.cpp
  src/devices/ufs_disk.cpp
  src/devices/irq.cpp
  src/platform/qcom_cmddb.cpp
  src/cpu/cpu_state.cpp
  src/cpu/unicorn_cpu.cpp
  src/platform/kona.cpp
  src/core/boot_pipeline.cpp
)
target_include_directories(hwemu_core PUBLIC src ${UNICORN_INCLUDE_DIR})
target_link_libraries(hwemu_core PUBLIC
  LibLZMA::LibLZMA BZip2::BZip2 ZLIB::ZLIB
  ${UNICORN_LIBRARY} capstone::capstone)

# --- Main executable ---
add_executable(hollywood_emu src/main.cpp)
target_link_libraries(hollywood_emu PRIVATE hwemu_core)
if (MSVC)
  target_link_options(hollywood_emu PRIVATE /MAP)
endif()

# Copy configs next to the binary for runtime discovery.
add_custom_command(TARGET hollywood_emu POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory
          ${CMAKE_SOURCE_DIR}/configs $<TARGET_FILE_DIR:hollywood_emu>/configs)
