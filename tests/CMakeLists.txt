add_executable(omacalendar-domain-test
  test_domain.cpp
)
target_link_libraries(omacalendar-domain-test
  PRIVATE
    Qt6::Test
    Qt6::Core
    omacalendar_core
)
add_test(NAME domain_test COMMAND omacalendar-domain-test)

add_executable(omacalendar-database-test
  test_database.cpp
)
target_link_libraries(omacalendar-database-test
  PRIVATE
    Qt6::Test
    Qt6::Core
    Qt6::Sql
    omacalendar_core
)
add_test(NAME database_test COMMAND omacalendar-database-test)

add_executable(omacalendar-widget-queries-test
  test_widget_queries.cpp
)
target_link_libraries(omacalendar-widget-queries-test
  PRIVATE
    Qt6::Test
    Qt6::Core
    Qt6::Sql
    omacalendar_core
)
add_test(NAME widget_queries_test COMMAND omacalendar-widget-queries-test)

add_executable(omacalendar-recurrence-test
  test_recurrence.cpp
)
target_link_libraries(omacalendar-recurrence-test
  PRIVATE
    Qt6::Test
    Qt6::Core
    Qt6::Sql
    omacalendar_core
)
add_test(NAME recurrence_test COMMAND omacalendar-recurrence-test)

add_executable(omacalendar-ipc-test
  test_ipc.cpp
)
target_link_libraries(omacalendar-ipc-test
  PRIVATE
    Qt6::Test
    Qt6::Core
    Qt6::Network
    omacalendar_core
    omacalendar_ipc
)
add_test(NAME ipc_test COMMAND omacalendar-ipc-test)

add_executable(omacalendar-providers-test
  test_providers.cpp
)
target_link_libraries(omacalendar-providers-test
  PRIVATE
    Qt6::Test
    Qt6::Core
    omacalendar_sync
)
add_test(NAME providers_test COMMAND omacalendar-providers-test)

add_executable(omacalendar-sync-coordinator-test
  test_synccoordinator.cpp
)
target_link_libraries(omacalendar-sync-coordinator-test
  PRIVATE
    Qt6::Test
    Qt6::Core
    omacalendar_sync
)
add_test(NAME sync_coordinator_test COMMAND omacalendar-sync-coordinator-test)

add_executable(omacalendar-ics-test
  test_ics.cpp
)
target_link_libraries(omacalendar-ics-test
  PRIVATE
    Qt6::Test
    Qt6::Core
    Qt6::Network
    omacalendar_sync
)
add_test(NAME ics_test COMMAND omacalendar-ics-test)

add_executable(omacalendar-secretstore-test
  test_secretstore.cpp
)
target_link_libraries(omacalendar-secretstore-test
  PRIVATE
    Qt6::Test
    Qt6::Core
    omacalendar_core
)
add_test(NAME secretstore_test COMMAND omacalendar-secretstore-test)

add_executable(omacalendar-googleauth-async-test
  test_googleauth.cpp
)
target_link_libraries(omacalendar-googleauth-async-test
  PRIVATE
    Qt6::Test
    Qt6::Core
    Qt6::Network
    omacalendar_sync
)
add_test(NAME googleauth_async_test COMMAND omacalendar-googleauth-async-test)

add_executable(omacalendar-caldav-test
  test_caldav.cpp
)
target_link_libraries(omacalendar-caldav-test
  PRIVATE
    Qt6::Test
    Qt6::Core
    Qt6::Network
    omacalendar_sync
)
add_test(NAME caldav_test COMMAND omacalendar-caldav-test)

add_executable(omacalendar-reminder-test
  test_reminders.cpp
  ${CMAKE_SOURCE_DIR}/src/reminders/reminderscheduler.cpp
  ${CMAKE_SOURCE_DIR}/src/reminders/reminderscheduler.h
)
target_link_libraries(omacalendar-reminder-test
  PRIVATE
    Qt6::Test
    Qt6::Core
    Qt6::DBus
    omacalendar_core
)
add_test(NAME reminder_test COMMAND omacalendar-reminder-test)

add_executable(omacalendar-appcontroller-test
  test_appcontroller.cpp
  ${CMAKE_SOURCE_DIR}/src/app/applicationinstance.cpp
  ${CMAKE_SOURCE_DIR}/src/app/applicationinstance.h
  ${CMAKE_SOURCE_DIR}/src/app/appcontroller.cpp
  ${CMAKE_SOURCE_DIR}/src/app/appcontroller.h
  ${CMAKE_SOURCE_DIR}/src/app/presentationlistmodel.cpp
  ${CMAKE_SOURCE_DIR}/src/app/presentationlistmodel.h
  ${CMAKE_SOURCE_DIR}/src/app/startuprequest.cpp
  ${CMAKE_SOURCE_DIR}/src/app/startuprequest.h
)
target_link_libraries(omacalendar-appcontroller-test
  PRIVATE
    Qt6::Test
    Qt6::Core
    Qt6::Gui
    Qt6::Network
    omacalendar_core
    omacalendar_ipc
)
add_test(NAME appcontroller_test COMMAND omacalendar-appcontroller-test)

add_executable(omacalendar-presentation-list-model-test
  test_presentationlistmodel.cpp
  ${CMAKE_SOURCE_DIR}/src/app/presentationlistmodel.cpp
  ${CMAKE_SOURCE_DIR}/src/app/presentationlistmodel.h
)
target_link_libraries(omacalendar-presentation-list-model-test
  PRIVATE
    Qt6::Test
    Qt6::Core
    Qt6::Qml
)
target_include_directories(omacalendar-presentation-list-model-test
  PRIVATE ${CMAKE_SOURCE_DIR}/src
)
add_test(NAME presentation_list_model_test
         COMMAND omacalendar-presentation-list-model-test)

add_executable(omacalendar-themebridge-test
  test_themebridge.cpp
  ${CMAKE_SOURCE_DIR}/src/app/themebridge.cpp
  ${CMAKE_SOURCE_DIR}/src/app/themebridge.h
)
target_link_libraries(omacalendar-themebridge-test
  PRIVATE
    Qt6::Test
    Qt6::Core
    Qt6::Gui
)
target_include_directories(omacalendar-themebridge-test
  PRIVATE ${CMAKE_SOURCE_DIR}/src
)
add_test(NAME themebridge_test COMMAND omacalendar-themebridge-test)

add_test(NAME daemon_contract_test
  COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/test_daemon_contract.py
    --daemon $<TARGET_FILE:omacalendard>
    --cli $<TARGET_FILE:omacalendarctl>
)
set_tests_properties(daemon_contract_test PROPERTIES TIMEOUT 60)

if(TARGET Qt6::qmltestrunner)
  set(OMACALENDAR_QML_SMOKE_INPUT "${CMAKE_CURRENT_SOURCE_DIR}/qml")
  set(OMACALENDAR_QML_SMOKE_IMPORTS "${OMACALENDAR_QML_SMOKE_INPUT}/imports")
  foreach(scale IN ITEMS 1 1.25 2)
    string(REPLACE "." "_" scale_suffix "${scale}")
    add_test(NAME qml_desktop_smoke_${scale_suffix}
      COMMAND Qt6::qmltestrunner
        -input "${OMACALENDAR_QML_SMOKE_INPUT}"
        -import "${OMACALENDAR_QML_SMOKE_IMPORTS}"
    )
    set_tests_properties(qml_desktop_smoke_${scale_suffix} PROPERTIES
      TIMEOUT 60
      ENVIRONMENT
        "LC_ALL=C.UTF-8;TZ=UTC;QT_QPA_PLATFORM=offscreen;QT_QUICK_CONTROLS_STYLE=Basic;QT_QUICK_BACKEND=software;QSG_RHI_BACKEND=software;QT_SCALE_FACTOR=${scale};QT_SCALE_FACTOR_ROUNDING_POLICY=PassThrough;QML_DISABLE_DISK_CACHE=1"
    )
  endforeach()
else()
  message(STATUS "Qt qmltestrunner unavailable; desktop QML smoke tests disabled")
endif()

add_test(NAME release_performance_smoke
  COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_SOURCE_DIR}/scripts/performance/release_performance.py
    --daemon $<TARGET_FILE:omacalendard>
    --events 100000
    --warmups 1
    --samples 3
    --build-label ctest-smoke
)
set_tests_properties(release_performance_smoke PROPERTIES
  TIMEOUT 180
  RUN_SERIAL TRUE
  LABELS "performance"
)

option(OMACALENDAR_ENABLE_HARDWARE_PERF_TESTS
       "Enable timing-enforced release tests for reference Omarchy hardware" OFF)
if(OMACALENDAR_ENABLE_HARDWARE_PERF_TESTS)
  add_test(NAME release_performance_hardware_gate
    COMMAND ${Python3_EXECUTABLE}
      ${CMAKE_SOURCE_DIR}/scripts/performance/release_performance.py
      --daemon $<TARGET_FILE:omacalendard>
      --events 100000
      --warmups 5
      --samples 15
      --enforce-gates
      --build-label Release/reference-Omarchy
  )
  set_tests_properties(release_performance_hardware_gate PROPERTIES
    TIMEOUT 300
    RUN_SERIAL TRUE
    LABELS "performance;hardware-gate"
  )
endif()
