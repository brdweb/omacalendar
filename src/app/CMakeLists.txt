qt_add_executable(omacalendar
  main.cpp
  applicationinstance.cpp
  applicationinstance.h
  appcontroller.cpp
  appcontroller.h
  presentationlistmodel.cpp
  presentationlistmodel.h
  startuprequest.cpp
  startuprequest.h
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
    qml/components/AppAccordionSection.qml
    qml/components/AppCheckBox.qml
    qml/components/AppCloseButton.qml
    qml/components/AppColorPicker.qml
    qml/components/AppComboBox.qml
    qml/components/AppSpinBox.qml
    qml/components/AppTabButton.qml
    qml/components/AppTextField.qml
    qml/components/AccountSettingsDrawer.qml
    qml/components/ActivityPanel.qml
    qml/components/CalendarSidebar.qml
    qml/components/EmptyState.qml
    qml/components/EventChip.qml
    qml/components/EventEditor.qml
    qml/components/EventRow.qml
    qml/components/MonthCell.qml
    qml/components/MutationConfirmationDialog.qml
    qml/components/SectionLabel.qml
    qml/components/StatusBadge.qml
    qml/components/TimelineCanvas.qml
    qml/components/TimelineEvent.qml
    qml/views/AgendaView.qml
    qml/views/DayView.qml
    qml/views/MonthView.qml
    qml/views/WeekView.qml
    qml/views/YearView.qml
)

target_link_libraries(omacalendar
  PRIVATE
    omacalendar_core
    omacalendar_ipc
    Qt6::Core
    Qt6::Gui
    Qt6::Network
    Qt6::Qml
    Qt6::Quick
    Qt6::QuickControls2
)

install(TARGETS omacalendar
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
