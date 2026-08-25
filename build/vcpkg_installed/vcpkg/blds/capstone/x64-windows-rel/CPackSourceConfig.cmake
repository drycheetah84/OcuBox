# This file will be configured to contain variables for CPack. These variables
# should be set in the CMake list file of the project before CPack module is
# included. The list of available CPACK_xxx variables and their associated
# documentation may be obtained using
#  cpack --help-variable-list
#
# Some variables are common to all generators (e.g. CPACK_PACKAGE_NAME)
# and some are specific to a generator
# (e.g. CPACK_NSIS_EXTRA_INSTALL_COMMANDS). The generator specific variables
# usually begin with CPACK_<GENNAME>_xxxx.


set(CPACK_ARCHIVE_GID "-1")
set(CPACK_ARCHIVE_UID "-1")
set(CPACK_BINARY_7Z "OFF")
set(CPACK_BINARY_IFW "OFF")
set(CPACK_BINARY_INNOSETUP "OFF")
set(CPACK_BINARY_NSIS "ON")
set(CPACK_BINARY_NUGET "OFF")
set(CPACK_BINARY_WIX "OFF")
set(CPACK_BINARY_ZIP "OFF")
set(CPACK_BUILD_SOURCE_DIRS "D:/projects/eurka_emu/build/vcpkg_installed/vcpkg/blds/capstone/src/5.0.9-9b23fb962a.clean;D:/projects/eurka_emu/build/vcpkg_installed/vcpkg/blds/capstone/x64-windows-rel")
set(CPACK_CMAKE_GENERATOR "Ninja")
set(CPACK_COMPONENT_UNSPECIFIED_HIDDEN "TRUE")
set(CPACK_COMPONENT_UNSPECIFIED_REQUIRED "TRUE")
set(CPACK_DEBIAN_PACKAGE_ARCHITECTURE "AMD64")
set(CPACK_DEBIAN_PACKAGE_CONTROL_EXTRA "D:/projects/eurka_emu/build/vcpkg_installed/vcpkg/blds/capstone/src/5.0.9-9b23fb962a.clean/packages/deb/triggers")
set(CPACK_DEBIAN_PACKAGE_DEPENDS "libc6 (>= 2.2.5)")
set(CPACK_DEBIAN_PACKAGE_FILE_NAME "libcapstone-dev_5.0.9_AMD64")
set(CPACK_DEBIAN_PACKAGE_MULTIARCH "same")
set(CPACK_DEBIAN_PACKAGE_NAME "libcapstone-dev")
set(CPACK_DEBIAN_PACKAGE_ORIGINAL_MAINTAINER "Debian Security Tools <team+pkg-security@tracker.debian.org>")
set(CPACK_DEBIAN_PACKAGE_PRIORITY "optional")
set(CPACK_DEBIAN_PACKAGE_SECTION "libdevel")
set(CPACK_DEBIAN_PACKAGE_SOURCE "capstone")
set(CPACK_DEBIAN_PACKAGE_VERSION "5.0.9")
set(CPACK_DEFAULT_PACKAGE_DESCRIPTION_FILE "C:/Users/drych/AppData/Local/vcpkg/downloads/tools/cmake-4.3.2-windows/cmake-4.3.2-windows-x86_64/share/cmake-4.3/Templates/CPack.GenericDescription.txt")
set(CPACK_DEFAULT_PACKAGE_DESCRIPTION_SUMMARY "capstone built using CMake")
set(CPACK_DMG_PACKAGE_FILE_NAME "capstone-5.0.9")
set(CPACK_DMG_SLA_USE_RESOURCE_FILE_LICENSE "ON")
set(CPACK_GENERATOR "7Z;ZIP")
set(CPACK_IGNORE_FILES "/CVS/;/\\.svn/;/\\.bzr/;/\\.hg/;/\\.git/;\\.swp\$;\\.#;/#")
set(CPACK_INNOSETUP_ARCHITECTURE "x64")
set(CPACK_INSTALLED_DIRECTORIES "D:/projects/eurka_emu/build/vcpkg_installed/vcpkg/blds/capstone/src/5.0.9-9b23fb962a.clean;/")
set(CPACK_INSTALL_CMAKE_PROJECTS "")
set(CPACK_INSTALL_PREFIX "D:/projects/eurka_emu/build/vcpkg_installed/vcpkg/pkgs/capstone_x64-windows")
set(CPACK_MODULE_PATH "")
set(CPACK_NSIS_DISPLAY_NAME "capstone 5.0.9")
set(CPACK_NSIS_INSTALLER_ICON_CODE "")
set(CPACK_NSIS_INSTALLER_MUI_ICON_CODE "")
set(CPACK_NSIS_INSTALL_ROOT "$PROGRAMFILES64")
set(CPACK_NSIS_PACKAGE_NAME "capstone 5.0.9")
set(CPACK_NSIS_UNINSTALL_NAME "Uninstall")
set(CPACK_OUTPUT_CONFIG_FILE "D:/projects/eurka_emu/build/vcpkg_installed/vcpkg/blds/capstone/x64-windows-rel/CPackConfig.cmake")
set(CPACK_PACKAGE_CONTACT "Rot127 <unisono@quyllur.org>")
set(CPACK_PACKAGE_DEFAULT_LOCATION "/")
set(CPACK_PACKAGE_DESCRIPTION "Capstone is a lightweight multi-platform, multi-architecture disassembly framework. These are the development headers and libraries.
 Features:
 - Support hardware architectures: AArch64, ARM, Alpha, BPF, EVM, HPPA, LongArch, M680X, M68K, MOS65XX, Mips, PowerPC, RISCV, SH, Sparc, SystemZ, TMS320C64x, TriCore, WASM, x86, XCore, Xtensa.
 - Clean/simple/lightweight/intuitive architecture-neutral API.
 - Provide details on disassembled instructions (called \"decomposer\" by some others).
 - Provide some semantics of the disassembled instruction, such as list of implicit registers read & written.
 - Thread-safe by design.
 - Special support for embedding into firmware or OS kernel.
 - Distributed under the open source BSD license.")
set(CPACK_PACKAGE_DESCRIPTION_FILE "C:/Users/drych/AppData/Local/vcpkg/downloads/tools/cmake-4.3.2-windows/cmake-4.3.2-windows-x86_64/share/cmake-4.3/Templates/CPack.GenericDescription.txt")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Lightweight multi-architecture disassembly framework - devel files")
set(CPACK_PACKAGE_FILE_NAME "capstone-5.0.9-Source")
set(CPACK_PACKAGE_HOMEPAGE_URL "https://www.capstone-engine.org/")
set(CPACK_PACKAGE_INSTALL_DIRECTORY "capstone 5.0.9")
set(CPACK_PACKAGE_INSTALL_REGISTRY_KEY "capstone 5.0.9")
set(CPACK_PACKAGE_NAME "capstone")
set(CPACK_PACKAGE_RELOCATABLE "true")
set(CPACK_PACKAGE_VENDOR "Rot127")
set(CPACK_PACKAGE_VERSION "5.0.9")
set(CPACK_PACKAGE_VERSION_MAJOR "5")
set(CPACK_PACKAGE_VERSION_MINOR "0")
set(CPACK_PACKAGE_VERSION_PATCH "9")
set(CPACK_PROJECT_CONFIG_FILE "D:/projects/eurka_emu/build/vcpkg_installed/vcpkg/blds/capstone/src/5.0.9-9b23fb962a.clean/CPackConfig.cmake")
set(CPACK_RESOURCE_FILE_LICENSE "D:/projects/eurka_emu/build/vcpkg_installed/vcpkg/blds/capstone/src/5.0.9-9b23fb962a.clean/LICENSE.TXT")
set(CPACK_RESOURCE_FILE_README "C:/Users/drych/AppData/Local/vcpkg/downloads/tools/cmake-4.3.2-windows/cmake-4.3.2-windows-x86_64/share/cmake-4.3/Templates/CPack.GenericDescription.txt")
set(CPACK_RESOURCE_FILE_WELCOME "C:/Users/drych/AppData/Local/vcpkg/downloads/tools/cmake-4.3.2-windows/cmake-4.3.2-windows-x86_64/share/cmake-4.3/Templates/CPack.GenericWelcome.txt")
set(CPACK_RPM_CHANGELOG_FILE "D:/projects/eurka_emu/build/vcpkg_installed/vcpkg/blds/capstone/src/5.0.9-9b23fb962a.clean/ChangeLog")
set(CPACK_RPM_PACKAGE_ARCHITECTURE "AMD64")
set(CPACK_RPM_PACKAGE_DESCRIPTION "Capstone is a lightweight multi-platform, multi-architecture disassembly framework. These are the development headers and libraries.
 Features:
 - Support hardware architectures: AArch64, ARM, Alpha, BPF, EVM, HPPA, LongArch, M680X, M68K, MOS65XX, Mips, PowerPC, RISCV, SH, Sparc, SystemZ, TMS320C64x, TriCore, WASM, x86, XCore, Xtensa.
 - Clean/simple/lightweight/intuitive architecture-neutral API.
 - Provide details on disassembled instructions (called \"decomposer\" by some others).
 - Provide some semantics of the disassembled instruction, such as list of implicit registers read & written.
 - Thread-safe by design.
 - Special support for embedding into firmware or OS kernel.
 - Distributed under the open source BSD license.")
set(CPACK_RPM_PACKAGE_FILE_NAME "capstone-devel-5.0.9.AMD64")
set(CPACK_RPM_PACKAGE_GROUP "Development/Libraries")
set(CPACK_RPM_PACKAGE_LICENSE "BSD3, LLVM")
set(CPACK_RPM_PACKAGE_NAME "capstone-devel")
set(CPACK_RPM_PACKAGE_REQUIRES "glibc >= 2.2.5")
set(CPACK_RPM_PACKAGE_SOURCES "ON")
set(CPACK_RPM_PACKAGE_VERSION "5.0.9")
set(CPACK_RPM_POST_INSTALL_SCRIPT_FILE "D:/projects/eurka_emu/build/vcpkg_installed/vcpkg/blds/capstone/src/5.0.9-9b23fb962a.clean/packages/rpm/postinstall.sh")
set(CPACK_RPM_POST_UNINSTALL_SCRIPT_FILE "D:/projects/eurka_emu/build/vcpkg_installed/vcpkg/blds/capstone/src/5.0.9-9b23fb962a.clean/packages/rpm/postinstall.sh")
set(CPACK_SET_DESTDIR "OFF")
set(CPACK_SOURCE_7Z "ON")
set(CPACK_SOURCE_GENERATOR "7Z;ZIP")
set(CPACK_SOURCE_IGNORE_FILES "/CVS/;/\\.svn/;/\\.bzr/;/\\.hg/;/\\.git/;\\.swp\$;\\.#;/#")
set(CPACK_SOURCE_INSTALLED_DIRECTORIES "D:/projects/eurka_emu/build/vcpkg_installed/vcpkg/blds/capstone/src/5.0.9-9b23fb962a.clean;/")
set(CPACK_SOURCE_OUTPUT_CONFIG_FILE "D:/projects/eurka_emu/build/vcpkg_installed/vcpkg/blds/capstone/x64-windows-rel/CPackSourceConfig.cmake")
set(CPACK_SOURCE_PACKAGE_FILE_NAME "capstone-5.0.9-Source")
set(CPACK_SOURCE_TOPLEVEL_TAG "win64-Source")
set(CPACK_SOURCE_ZIP "ON")
set(CPACK_STRIP_FILES "")
set(CPACK_SYSTEM_NAME "win64")
set(CPACK_THREADS "1")
set(CPACK_TOPLEVEL_TAG "win64-Source")
set(CPACK_WIX_SIZEOF_VOID_P "8")

if(NOT CPACK_PROPERTIES_FILE)
  set(CPACK_PROPERTIES_FILE "D:/projects/eurka_emu/build/vcpkg_installed/vcpkg/blds/capstone/x64-windows-rel/CPackProperties.cmake")
endif()

if(EXISTS ${CPACK_PROPERTIES_FILE})
  include(${CPACK_PROPERTIES_FILE})
endif()
