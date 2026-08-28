cmake_minimum_required(VERSION 3.28)

project(OmaCalendar
  VERSION 0.1.0
  DESCRIPTION "A local-first calendar for Omarchy Linux"
  LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(OMACALENDAR_BUILD_APP "Build the Qt Quick desktop application" ON)
option(OMACALENDAR_BUILD_TESTS "Build automated tests" ON)

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
pkg_check_modules(LibIcal REQUIRED IMPORTED_TARGET libical)

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
  PUBLIC OMACALENDAR_VERSION="${PROJECT_VERSION}"
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
  src/sync/retrypolicy.cpp
  src/sync/retrypolicy.h
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
)
target_link_libraries(omacalendard
  PRIVATE omacalendar_core omacalendar_ipc omacalendar_sync
)

add_executable(omacalendarctl src/cli/main.cpp)
target_link_libraries(omacalendarctl
  PRIVATE omacalendar_core omacalendar_ipc Qt6::Core
)

if(OMACALENDAR_BUILD_APP)
  add_subdirectory(src/app)
endif()

include(GNUInstallDirs)
install(TARGETS omacalendard omacalendarctl
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
install(FILES packaging/systemd/omacalendard.service
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/systemd/user
)
install(FILES packaging/omacalendar.desktop
  DESTINATION ${CMAKE_INSTALL_DATADIR}/applications
)

if(OMACALENDAR_BUILD_TESTS)
  enable_testing()
  find_package(Qt6 6.8 REQUIRED COMPONENTS Test)
  add_subdirectory(tests)
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  foreach(target omacalendar_core omacalendar_ipc omacalendar_sync
                 omacalendard omacalendarctl)
    target_compile_options(${target} PRIVATE
      -Wall -Wextra -Wpedantic -Wconversion -Wshadow
    )
  endforeach()
  if(TARGET omacalendar)
    target_compile_options(omacalendar PRIVATE
      -Wall -Wextra -Wpedantic -Wconversion -Wshadow
    )
  endif()
endif()
