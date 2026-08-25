# Install script for directory: D:/projects/eurka_emu/build/vcpkg_installed/vcpkg/blds/unicorn/src/2.1.4-76b3dff2cc.clean

# Set the install prefix
if(NOT DEFINED CMAKE_INSTALL_PREFIX)
  set(CMAKE_INSTALL_PREFIX "D:/projects/eurka_emu/build/vcpkg_installed/vcpkg/pkgs/unicorn_x64-windows/debug")
endif()
string(REGEX REPLACE "/$" "" CMAKE_INSTALL_PREFIX "${CMAKE_INSTALL_PREFIX}")

# Set the install configuration name.
if(NOT DEFINED CMAKE_INSTALL_CONFIG_NAME)
  if(BUILD_TYPE)
    string(REGEX REPLACE "^[^A-Za-z0-9_]+" ""
           CMAKE_INSTALL_CONFIG_NAME "${BUILD_TYPE}")
  else()
    set(CMAKE_INSTALL_CONFIG_NAME "Debug")
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

# Set path to fallback-tool for dependency-resolution.
if(NOT DEFINED CMAKE_OBJDUMP)
  set(CMAKE_OBJDUMP "CMAKE_OBJDUMP-NOTFOUND")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib" TYPE STATIC_LIBRARY OPTIONAL FILES "D:/projects/eurka_emu/build/vcpkg_installed/vcpkg/blds/unicorn/x64-windows-dbg/unicorn.lib")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/bin" TYPE SHARED_LIBRARY FILES "D:/projects/eurka_emu/build/vcpkg_installed/vcpkg/blds/unicorn/x64-windows-dbg/unicorn.dll")
  if(EXISTS "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/unicorn.dll" AND
     NOT IS_SYMLINK "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/unicorn.dll")
    if(CMAKE_INSTALL_DO_STRIP)
      execute_process(COMMAND "CMAKE_STRIP-NOTFOUND" "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/unicorn.dll")
    endif()
  endif()
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/include/unicorn" TYPE FILE FILES
    "D:/projects/eurka_emu/build/vcpkg_installed/vcpkg/blds/unicorn/src/2.1.4-76b3dff2cc.clean/include/unicorn/arm.h"
    "D:/projects/eurka_emu/build/vcpkg_installed/vcpkg/blds/unicorn/src/2.1.4-76b3dff2cc.clean/include/unicorn/arm64.h"
    "D:/projects/eurka_emu/build/vcpkg_installed/vcpkg/blds/unicorn/src/2.1.4-76b3dff2cc.clean/include/unicorn/m68k.h"
    "D:/projects/eurka_emu/build/vcpkg_installed/vcpkg/blds/unicorn/src/2.1.4-76b3dff2cc.clean/include/unicorn/mips.h"
    "D:/projects/eurka_emu/build/vcpkg_installed/vcpkg/blds/unicorn/src/2.1.4-76b3dff2cc.clean/include/unicorn/platform.h"
    "D:/projects/eurka_emu/build/vcpkg_installed/vcpkg/blds/unicorn/src/2.1.4-76b3dff2cc.clean/include/unicorn/ppc.h"
    "D:/projects/eurka_emu/build/vcpkg_installed/vcpkg/blds/unicorn/src/2.1.4-76b3dff2cc.clean/include/unicorn/riscv.h"
    "D:/projects/eurka_emu/build/vcpkg_installed/vcpkg/blds/unicorn/src/2.1.4-76b3dff2cc.clean/include/unicorn/s390x.h"
    "D:/projects/eurka_emu/build/vcpkg_installed/vcpkg/blds/unicorn/src/2.1.4-76b3dff2cc.clean/include/unicorn/sparc.h"
    "D:/projects/eurka_emu/build/vcpkg_installed/vcpkg/blds/unicorn/src/2.1.4-76b3dff2cc.clean/include/unicorn/tricore.h"
    "D:/projects/eurka_emu/build/vcpkg_installed/vcpkg/blds/unicorn/src/2.1.4-76b3dff2cc.clean/include/unicorn/unicorn.h"
    "D:/projects/eurka_emu/build/vcpkg_installed/vcpkg/blds/unicorn/src/2.1.4-76b3dff2cc.clean/include/unicorn/x86.h"
    )
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib/pkgconfig" TYPE FILE FILES "D:/projects/eurka_emu/build/vcpkg_installed/vcpkg/blds/unicorn/x64-windows-dbg/unicorn.pc")
endif()

string(REPLACE ";" "\n" CMAKE_INSTALL_MANIFEST_CONTENT
       "${CMAKE_INSTALL_MANIFEST_FILES}")
if(CMAKE_INSTALL_LOCAL_ONLY)
  file(WRITE "D:/projects/eurka_emu/build/vcpkg_installed/vcpkg/blds/unicorn/x64-windows-dbg/install_local_manifest.txt"
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
  file(WRITE "D:/projects/eurka_emu/build/vcpkg_installed/vcpkg/blds/unicorn/x64-windows-dbg/${CMAKE_INSTALL_MANIFEST}"
     "${CMAKE_INSTALL_MANIFEST_CONTENT}")
endif()
