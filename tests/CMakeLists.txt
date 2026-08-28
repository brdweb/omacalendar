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
