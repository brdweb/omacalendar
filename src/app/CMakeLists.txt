qt_add_executable(omacalendar
  main.cpp
  appcontroller.cpp
  appcontroller.h
  themebridge.cpp
  themebridge.h
)

set_source_files_properties(qml/Theme.qml PROPERTIES
  QT_QML_SINGLETON_TYPE TRUE
)

qt_add_qml_module(omacalendar
  URI OmaCalendar
  VERSION 1.0
  QML_FILES
    qml/Main.qml
    qml/Theme.qml
    qml/components/AppButton.qml
    qml/components/AppTextField.qml
    qml/components/EventEditor.qml
    qml/components/EventRow.qml
    qml/components/MonthCell.qml
    qml/components/SectionLabel.qml
)

target_link_libraries(omacalendar
  PRIVATE
    omacalendar_core
    omacalendar_ipc
    Qt6::Core
    Qt6::Gui
    Qt6::Qml
    Qt6::Quick
    Qt6::QuickControls2
)

install(TARGETS omacalendar
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
