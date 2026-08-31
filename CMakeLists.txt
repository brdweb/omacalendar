cmake_minimum_required(VERSION 3.28)

project(OmaCalendar
  VERSION 1.0.0
  DESCRIPTION "A local-first calendar for Omarchy Linux"
  LANGUAGES CXX
)

include(GNUInstallDirs)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(OMACALENDAR_BUILD_APP "Build the Qt Quick desktop application" ON)
option(OMACALENDAR_BUILD_TESTS "Build automated tests" ON)
option(OMACALENDAR_ENABLE_WERROR "Treat compiler warnings as errors" OFF)
option(OMACALENDAR_ENABLE_SANITIZERS
       "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
set(OMACALENDAR_VERSION_SUFFIX "-alpha" CACHE STRING
    "Suffix appended to the numeric project version (empty for a release)")
set(OMACALENDAR_FULL_VERSION "${PROJECT_VERSION}${OMACALENDAR_VERSION_SUFFIX}")

if(OMACALENDAR_ENABLE_SANITIZERS)
  if(NOT CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    message(FATAL_ERROR "OMACALENDAR_ENABLE_SANITIZERS requires GCC or Clang")
  endif()
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
endif()

find_package(Qt6 6.8 REQUIRED COMPONENTS
  Core
  DBus
  Gui
  Network
  NetworkAuth
  Qml
  Quick
  QuickControls2
  Sql
)
find_package(PkgConfig REQUIRED)
pkg_check_modules(LibIcal REQUIRED IMPORTED_TARGET libical>=4.0)

qt_standard_project_setup(REQUIRES 6.8)

add_library(omacalendar_core STATIC
  src/core/database.cpp
  src/core/database.h
  src/core/domain.cpp
  src/core/domain.h
  src/core/paths.cpp
  src/core/paths.h
  src/core/recurrenceexpander.cpp
  src/core/recurrenceexpander.h
  src/core/secretstore.cpp
  src/core/secretstore.h
  src/core/widgeteventquery.cpp
  src/core/widgeteventquery.h
)

target_include_directories(omacalendar_core PUBLIC src)
target_link_libraries(omacalendar_core
  PUBLIC
    Qt6::Core
    Qt6::Sql
  PRIVATE
    Qt6::DBus
    PkgConfig::LibIcal
)
target_compile_definitions(omacalendar_core
  PUBLIC OMACALENDAR_VERSION="${OMACALENDAR_FULL_VERSION}"
)

add_library(omacalendar_ipc STATIC
  src/ipc/ipcclient.cpp
  src/ipc/ipcclient.h
  src/ipc/ipcprotocol.cpp
  src/ipc/ipcprotocol.h
  src/ipc/ipcserver.cpp
  src/ipc/ipcserver.h
  src/ipc/requestrouter.cpp
  src/ipc/requestrouter.h
)

target_include_directories(omacalendar_ipc PUBLIC src)
target_link_libraries(omacalendar_ipc PUBLIC Qt6::Core Qt6::Network)

add_library(omacalendar_sync STATIC
  src/sync/provider.h
  src/sync/localprovider.cpp
  src/sync/localprovider.h
  src/sync/retrypolicy.cpp
  src/sync/retrypolicy.h
  src/sync/synccoordinator.cpp
  src/sync/synccoordinator.h
  src/providers/caldav/caldavclient.cpp
  src/providers/caldav/caldavclient.h
  src/providers/caldav/caldavsync.cpp
  src/providers/caldav/caldavsync.h
  src/providers/caldav/caldavxml.cpp
  src/providers/caldav/caldavxml.h
  src/providers/caldav/icalcodec.cpp
  src/providers/caldav/icalcodec.h
  src/providers/google/googleauth.cpp
  src/providers/google/googleauth.h
  src/providers/google/googleclient.cpp
  src/providers/google/googleclient.h
  src/providers/google/googlemapper.cpp
  src/providers/google/googlemapper.h
  src/providers/google/googleoauthconfig.h
  src/providers/google/googlesync.cpp
  src/providers/google/googlesync.h
  src/providers/ics/icsservice.cpp
  src/providers/ics/icsservice.h
)
target_include_directories(omacalendar_sync PUBLIC src)
target_link_libraries(omacalendar_sync
  PUBLIC
    omacalendar_core
    Qt6::Core
    Qt6::Network
    Qt6::NetworkAuth
  PRIVATE
    PkgConfig::LibIcal
)

add_executable(omacalendard
  src/daemon/daemon.cpp
  src/daemon/daemon.h
  src/daemon/main.cpp
  src/reminders/reminderscheduler.cpp
  src/reminders/reminderscheduler.h
)
target_link_libraries(omacalendard
  PRIVATE omacalendar_core omacalendar_ipc omacalendar_sync Qt6::DBus
)

add_executable(omacalendarctl src/cli/main.cpp)
target_link_libraries(omacalendarctl
  PRIVATE omacalendar_core omacalendar_ipc Qt6::Core
)

if(OMACALENDAR_BUILD_APP)
  add_subdirectory(src/app)
endif()

install(TARGETS omacalendard omacalendarctl
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
install(PROGRAMS scripts/omacalendar-widgetctl
  DESTINATION ${CMAKE_INSTALL_BINDIR}
)

configure_file(
  packaging/systemd/omacalendard.service.in
  ${CMAKE_CURRENT_BINARY_DIR}/packaging/omacalendard.service
  @ONLY
)
configure_file(
  packaging/systemd/omacalendard.socket.in
  ${CMAKE_CURRENT_BINARY_DIR}/packaging/omacalendard.socket
  @ONLY
)

install(FILES
  ${CMAKE_CURRENT_BINARY_DIR}/packaging/omacalendard.service
  ${CMAKE_CURRENT_BINARY_DIR}/packaging/omacalendard.socket
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/systemd/user
)
install(FILES packaging/org.omacalendar.OmaCalendar.desktop
  DESTINATION ${CMAKE_INSTALL_DATADIR}/applications
)
install(FILES packaging/org.omacalendar.OmaCalendar.metainfo.xml
  DESTINATION ${CMAKE_INSTALL_DATADIR}/metainfo
)
install(FILES packaging/icons/org.omacalendar.OmaCalendar.svg
  DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps
)
install(FILES LICENSE README.md CHANGELOG.md
  DESTINATION ${CMAKE_INSTALL_DOCDIR}
)
install(FILES SECURITY.md SUPPORT.md
  docs/BACKUP_AND_RECOVERY.md
  docs/COMPATIBILITY.md
  docs/PRIVACY.md
  docs/UNINSTALL.md
  DESTINATION ${CMAKE_INSTALL_DOCDIR}
)

if(OMACALENDAR_BUILD_TESTS)
  enable_testing()
  find_package(Qt6 6.8 REQUIRED COMPONENTS Test)
  find_package(Python3 REQUIRED COMPONENTS Interpreter)
  add_subdirectory(tests)
  add_test(NAME widget_activation_test
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/tests/test_widgetctl.py
  )
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  set(OMACALENDAR_WARNING_FLAGS
    -Wall -Wextra -Wpedantic -Wconversion -Wshadow
  )
  if(OMACALENDAR_ENABLE_WERROR)
    list(APPEND OMACALENDAR_WARNING_FLAGS -Werror)
  endif()
  set(OMACALENDAR_WARNING_TARGETS
    omacalendar_core
    omacalendar_ipc
    omacalendar_sync
    omacalendard
    omacalendarctl
  )
  if(OMACALENDAR_BUILD_TESTS)
    get_property(OMACALENDAR_TEST_TARGETS DIRECTORY tests
                 PROPERTY BUILDSYSTEM_TARGETS)
    list(APPEND OMACALENDAR_WARNING_TARGETS ${OMACALENDAR_TEST_TARGETS})
  endif()
  foreach(target IN LISTS OMACALENDAR_WARNING_TARGETS)
    target_compile_options(${target} PRIVATE ${OMACALENDAR_WARNING_FLAGS})
  endforeach()
  if(TARGET omacalendar)
    target_compile_options(omacalendar PRIVATE ${OMACALENDAR_WARNING_FLAGS})
    if(OMACALENDAR_ENABLE_WERROR)
      # qmlcachegen can emit narrowing conversions in generated Qt code. Keep
      # project warnings fatal without treating this generated-code warning as
      # a release blocker.
      target_compile_options(omacalendar PRIVATE -Wno-error=conversion)
    endif()
  endif()
endif()

set(CPACK_PACKAGE_NAME "OmaCalendar")
set(CPACK_PACKAGE_VENDOR "OmaCalendar contributors")
set(CPACK_PACKAGE_CONTACT "https://github.com/brdweb/omacalendar/issues")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Local-first calendar for Omarchy Linux")
set(CPACK_PACKAGE_HOMEPAGE_URL "https://github.com/brdweb/omacalendar")
set(CPACK_PACKAGE_VERSION "${OMACALENDAR_FULL_VERSION}")
set(CPACK_PACKAGE_FILE_NAME
    "omacalendar-${OMACALENDAR_FULL_VERSION}-${CMAKE_SYSTEM_NAME}-${CMAKE_SYSTEM_PROCESSOR}")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")
set(CPACK_GENERATOR "TGZ")
set(CPACK_SOURCE_GENERATOR "TGZ")
set(CPACK_SOURCE_IGNORE_FILES
  "/\\.git/"
  "/build[^/]*/"
  "/\\.cache/"
  "\\.user$"
  "~$"
)
include(CPack)
