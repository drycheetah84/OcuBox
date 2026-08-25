# Install script for directory: D:/projects/eurka_emu/build/vcpkg_installed/vcpkg/blds/capstone/src/5.0.9-9b23fb962a.clean

# Set the install prefix
if(NOT DEFINED CMAKE_INSTALL_PREFIX)
  set(CMAKE_INSTALL_PREFIX "D:/projects/eurka_emu/build/vcpkg_installed/vcpkg/pkgs/capstone_x64-windows")
endif()
string(REGEX REPLACE "/$" "" CMAKE_INSTALL_PREFIX "${CMAKE_INSTALL_PREFIX}")

# Set the install configuration name.
if(NOT DEFINED CMAKE_INSTALL_CONFIG_NAME)
  if(BUILD_TYPE)
    string(REGEX REPLACE "^[^A-Za-z0-9_]+" ""
           CMAKE_INSTALL_CONFIG_NAME "${BUILD_TYPE}")
  else()
    set(CMAKE_INSTALL_CONFIG_NAME "Release")
  endif()
  message(STATUS "Install configuration: \"${CMAKE_INSTALL_CONFIG_NAME}\"")
endif()

# Set the component getting installed.
if(NOT CMAKE_INSTALL_COMPONENT)
  if(COMPONENT)
    message(STATUS "Install component: \"${COMPONENT}\"")
    set(CMAKE_INSTALL_COMPONENT "${COMPONENT}")
  else()
    set(CMAKE_INSTALL_COMPONENT)
  endif()
endif()

# Is this installation the result of a crosscompile?
if(NOT DEFINED CMAKE_CROSSCOMPILING)
  set(CMAKE_CROSSCOMPILING "OFF")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/include/capstone" TYPE FILE FILES
    "D:/projects/eurka_emu/build/vcpkg_installed/vcpkg/blds/capstone/src/5.0.9-9b23fb962a.clean/include/capstone/arm64.h"
    "D:/projects/eurka_emu/build/vcpkg_installed/vcpkg/blds/capstone/src/5.0.9-9b23fb962a.clean/include/capstone/arm.h"
    "D:/projects/eurka_emu/build/vcpkg_installed/vcpkg/blds/capstone/src/5.0.9-9b23fb962a.clean/include/capstone/capstone.h"
    "D:/projects/eurka_emu/build/vcpkg_installed/vcpkg/blds/capstone/src/5.0.9-9b23fb962a.clean/include/capstone/evm.h"
    "D:/projects/eurka_emu/build/vcpkg_installed/vcpkg/blds/capstone/src/5.0.9-9b23fb962a.clean/include/capstone/wasm.h"
    "D:/projects/eurka_emu/build/vcpkg_installed/vcpkg/blds/capstone/src/5.0.9-9b23fb962a.clean/include/capstone/mips.h"
    "D:/projects/eurka_emu/build/vcpkg_installed/vcpkg/blds/capstone/src/5.0.9-9b23fb962a.clean/include/capstone/ppc.h"
    "D:/projects/eurka_emu/build/vcpkg_installed/vcpkg/blds/capstone/src/5.0.9-9b23fb962a.clean/include/capstone/x86.h"
    "D:/projects/eurka_emu/build/vcpkg_installed/vcpkg/blds/capstone/src/5.0.9-9b23fb962a.clean/include/capstone/sparc.h"
    "D:/projects/eurka_emu/build/vcpkg_installed/vcpkg/blds/capstone/src/5.0.9-9b23fb962a.clean/include/capstone/systemz.h"
    "D:/projects/eurka_emu/build/vcpkg_installed/vcpkg/blds/capstone/src/5.0.9-9b23fb962a.clean/include/capstone/xcore.h"
    "D:/projects/eurka_emu/build/vcpkg_installed/vcpkg/blds/capstone/src/5.0.9-9b23fb962a.clean/include/capstone/m68k.h"
    "D:/projects/eurka_emu/build/vcpkg_installed/vcpkg/blds/capstone/src/5.0.9-9b23fb962a.clean/include/capstone/tms320c64x.h"
    "D:/projects/eurka_emu/build/vcpkg_installed/vcpkg/blds/capstone/src/5.0.9-9b23fb962a.clean/include/capstone/m680x.h"
    "D:/projects/eurka_emu/build/vcpkg_installed/vcpkg/blds/capstone/src/5.0.9-9b23fb962a.clean/include/capstone/mos65xx.h"
    "D:/projects/eurka_emu/build/vcpkg_installed/vcpkg/blds/capstone/src/5.0.9-9b23fb962a.clean/include/capstone/bpf.h"
    "D:/projects/eurka_emu/build/vcpkg_installed/vcpkg/blds/capstone/src/5.0.9-9b23fb962a.clean/include/capstone/riscv.h"
    "D:/projects/eurka_emu/build/vcpkg_installed/vcpkg/blds/capstone/src/5.0.9-9b23fb962a.clean/include/capstone/sh.h"
    "D:/projects/eurka_emu/build/vcpkg_installed/vcpkg/blds/capstone/src/5.0.9-9b23fb962a.clean/include/capstone/tricore.h"
    "D:/projects/eurka_emu/build/vcpkg_installed/vcpkg/blds/capstone/src/5.0.9-9b23fb962a.clean/include/capstone/platform.h"
    )
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib/pkgconfig" TYPE FILE FILES "D:/projects/eurka_emu/build/vcpkg_installed/vcpkg/blds/capstone/x64-windows-rel/capstone.pc")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib/cmake/capstone" TYPE FILE FILES
    "D:/projects/eurka_emu/build/vcpkg_installed/vcpkg/blds/capstone/x64-windows-rel/capstone-config.cmake"
    "D:/projects/eurka_emu/build/vcpkg_installed/vcpkg/blds/capstone/x64-windows-rel/capstone-config-version.cmake"
    )
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib" TYPE STATIC_LIBRARY OPTIONAL FILES "D:/projects/eurka_emu/build/vcpkg_installed/vcpkg/blds/capstone/x64-windows-rel/capstone.lib")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/bin" TYPE SHARED_LIBRARY FILES "D:/projects/eurka_emu/build/vcpkg_installed/vcpkg/blds/capstone/x64-windows-rel/capstone.dll")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  if(EXISTS "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib/cmake/capstone/capstone-targets.cmake")
    file(DIFFERENT _cmake_export_file_changed FILES
         "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib/cmake/capstone/capstone-targets.cmake"
         "D:/projects/eurka_emu/build/vcpkg_installed/vcpkg/blds/capstone/x64-windows-rel/CMakeFiles/Export/a9b5d84ad64c19ad8b017b7d94696c98/capstone-targets.cmake")
    if(_cmake_export_file_changed)
      file(GLOB _cmake_old_config_files "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib/cmake/capstone/capstone-targets-*.cmake")
      if(_cmake_old_config_files)
        string(REPLACE ";" ", " _cmake_old_config_files_text "${_cmake_old_config_files}")
        message(STATUS "Old export file \"$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib/cmake/capstone/capstone-targets.cmake\" will be replaced.  Removing files [${_cmake_old_config_files_text}].")
        unset(_cmake_old_config_files_text)
        file(REMOVE ${_cmake_old_config_files})
      endif()
      unset(_cmake_old_config_files)
    endif()
    unset(_cmake_export_file_changed)
  endif()
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib/cmake/capstone" TYPE FILE FILES "D:/projects/eurka_emu/build/vcpkg_installed/vcpkg/blds/capstone/x64-windows-rel/CMakeFiles/Export/a9b5d84ad64c19ad8b017b7d94696c98/capstone-targets.cmake")
  if(CMAKE_INSTALL_CONFIG_NAME MATCHES "^([Rr][Ee][Ll][Ee][Aa][Ss][Ee])$")
    file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib/cmake/capstone" TYPE FILE FILES "D:/projects/eurka_emu/build/vcpkg_installed/vcpkg/blds/capstone/x64-windows-rel/CMakeFiles/Export/a9b5d84ad64c19ad8b017b7d94696c98/capstone-targets-release.cmake")
  endif()
endif()

string(REPLACE ";" "\n" CMAKE_INSTALL_MANIFEST_CONTENT
       "${CMAKE_INSTALL_MANIFEST_FILES}")
if(CMAKE_INSTALL_LOCAL_ONLY)
  file(WRITE "D:/projects/eurka_emu/build/vcpkg_installed/vcpkg/blds/capstone/x64-windows-rel/install_local_manifest.txt"
     "${CMAKE_INSTALL_MANIFEST_CONTENT}")
endif()
if(CMAKE_INSTALL_COMPONENT)
  if(CMAKE_INSTALL_COMPONENT MATCHES "^[a-zA-Z0-9_.+-]+$")
    set(CMAKE_INSTALL_MANIFEST "install_manifest_${CMAKE_INSTALL_COMPONENT}.txt")
  else()
    string(MD5 CMAKE_INST_COMP_HASH "${CMAKE_INSTALL_COMPONENT}")
    set(CMAKE_INSTALL_MANIFEST "install_manifest_${CMAKE_INST_COMP_HASH}.txt")
    unset(CMAKE_INST_COMP_HASH)
  endif()
else()
  set(CMAKE_INSTALL_MANIFEST "install_manifest.txt")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  file(WRITE "D:/projects/eurka_emu/build/vcpkg_installed/vcpkg/blds/capstone/x64-windows-rel/${CMAKE_INSTALL_MANIFEST}"
     "${CMAKE_INSTALL_MANIFEST_CONTENT}")
endif()
