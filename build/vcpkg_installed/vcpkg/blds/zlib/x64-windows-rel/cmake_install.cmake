# Install script for directory: D:/projects/eurka_emu/build/vcpkg_installed/vcpkg/blds/zlib/src/v1.3.2-c19a0bfd23.clean

# Set the install prefix
if(NOT DEFINED CMAKE_INSTALL_PREFIX)
  set(CMAKE_INSTALL_PREFIX "D:/projects/eurka_emu/build/vcpkg_installed/vcpkg/pkgs/zlib_x64-windows")
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

if(CMAKE_INSTALL_COMPONENT STREQUAL "Runtime" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib" TYPE STATIC_LIBRARY OPTIONAL FILES "D:/projects/eurka_emu/build/vcpkg_installed/vcpkg/blds/zlib/x64-windows-rel/z.lib")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Runtime" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/bin" TYPE SHARED_LIBRARY FILES "D:/projects/eurka_emu/build/vcpkg_installed/vcpkg/blds/zlib/x64-windows-rel/z.dll")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  if(EXISTS "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib/cmake/zlib/ZLIB-shared.cmake")
    file(DIFFERENT _cmake_export_file_changed FILES
         "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib/cmake/zlib/ZLIB-shared.cmake"
         "D:/projects/eurka_emu/build/vcpkg_installed/vcpkg/blds/zlib/x64-windows-rel/CMakeFiles/Export/93f1ef598f1f2f8b07b376ab081bbce6/ZLIB-shared.cmake")
    if(_cmake_export_file_changed)
      file(GLOB _cmake_old_config_files "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib/cmake/zlib/ZLIB-shared-*.cmake")
      if(_cmake_old_config_files)
        string(REPLACE ";" ", " _cmake_old_config_files_text "${_cmake_old_config_files}")
        message(STATUS "Old export file \"$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib/cmake/zlib/ZLIB-shared.cmake\" will be replaced.  Removing files [${_cmake_old_config_files_text}].")
        unset(_cmake_old_config_files_text)
        file(REMOVE ${_cmake_old_config_files})
      endif()
      unset(_cmake_old_config_files)
    endif()
    unset(_cmake_export_file_changed)
  endif()
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib/cmake/zlib" TYPE FILE FILES "D:/projects/eurka_emu/build/vcpkg_installed/vcpkg/blds/zlib/x64-windows-rel/CMakeFiles/Export/93f1ef598f1f2f8b07b376ab081bbce6/ZLIB-shared.cmake")
  if(CMAKE_INSTALL_CONFIG_NAME MATCHES "^([Rr][Ee][Ll][Ee][Aa][Ss][Ee])$")
    file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib/cmake/zlib" TYPE FILE FILES "D:/projects/eurka_emu/build/vcpkg_installed/vcpkg/blds/zlib/x64-windows-rel/CMakeFiles/Export/93f1ef598f1f2f8b07b376ab081bbce6/ZLIB-shared-release.cmake")
  endif()
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Runtime" OR NOT CMAKE_INSTALL_COMPONENT)
  if(CMAKE_INSTALL_CONFIG_NAME MATCHES "^([Dd][Ee][Bb][Uu][Gg]|[Oo][Rr]|[Rr][Ee][Ll][Ww][Ii][Tt][Hh][Dd][Ee][Bb][Ii][Nn][Ff][Oo])$")
    file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/bin" TYPE FILE OPTIONAL FILES "D:/projects/eurka_emu/build/vcpkg_installed/vcpkg/blds/zlib/x64-windows-rel/z.pdb")
  endif()
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib/cmake/zlib" TYPE FILE FILES
    "D:/projects/eurka_emu/build/vcpkg_installed/vcpkg/blds/zlib/x64-windows-rel/ZLIBConfig.cmake"
    "D:/projects/eurka_emu/build/vcpkg_installed/vcpkg/blds/zlib/x64-windows-rel/ZLIBConfigVersion.cmake"
    )
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Development" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/include" TYPE FILE FILES
    "D:/projects/eurka_emu/build/vcpkg_installed/vcpkg/blds/zlib/x64-windows-rel/zconf.h"
    "D:/projects/eurka_emu/build/vcpkg_installed/vcpkg/blds/zlib/src/v1.3.2-c19a0bfd23.clean/zlib.h"
    )
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Docs" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/share/man/man3" TYPE FILE FILES "D:/projects/eurka_emu/build/vcpkg_installed/vcpkg/blds/zlib/src/v1.3.2-c19a0bfd23.clean/zlib.3")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Docs" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/share/doc/zlib/zlib" TYPE FILE FILES
    "D:/projects/eurka_emu/build/vcpkg_installed/vcpkg/blds/zlib/src/v1.3.2-c19a0bfd23.clean/LICENSE"
    "D:/projects/eurka_emu/build/vcpkg_installed/vcpkg/blds/zlib/src/v1.3.2-c19a0bfd23.clean/doc/algorithm.txt"
    "D:/projects/eurka_emu/build/vcpkg_installed/vcpkg/blds/zlib/src/v1.3.2-c19a0bfd23.clean/doc/crc-doc.1.0.pdf"
    "D:/projects/eurka_emu/build/vcpkg_installed/vcpkg/blds/zlib/src/v1.3.2-c19a0bfd23.clean/doc/rfc1950.txt"
    "D:/projects/eurka_emu/build/vcpkg_installed/vcpkg/blds/zlib/src/v1.3.2-c19a0bfd23.clean/doc/rfc1951.txt"
    "D:/projects/eurka_emu/build/vcpkg_installed/vcpkg/blds/zlib/src/v1.3.2-c19a0bfd23.clean/doc/rfc1952.txt"
    "D:/projects/eurka_emu/build/vcpkg_installed/vcpkg/blds/zlib/src/v1.3.2-c19a0bfd23.clean/doc/txtvsbin.txt"
    )
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Development" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib/pkgconfig" TYPE FILE FILES "D:/projects/eurka_emu/build/vcpkg_installed/vcpkg/blds/zlib/x64-windows-rel/zlib.pc")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("D:/projects/eurka_emu/build/vcpkg_installed/vcpkg/blds/zlib/x64-windows-rel/contrib/cmake_install.cmake")
endif()

string(REPLACE ";" "\n" CMAKE_INSTALL_MANIFEST_CONTENT
       "${CMAKE_INSTALL_MANIFEST_FILES}")
if(CMAKE_INSTALL_LOCAL_ONLY)
  file(WRITE "D:/projects/eurka_emu/build/vcpkg_installed/vcpkg/blds/zlib/x64-windows-rel/install_local_manifest.txt"
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
  file(WRITE "D:/projects/eurka_emu/build/vcpkg_installed/vcpkg/blds/zlib/x64-windows-rel/${CMAKE_INSTALL_MANIFEST}"
     "${CMAKE_INSTALL_MANIFEST_CONTENT}")
endif()
