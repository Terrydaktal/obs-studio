add_executable(test-recording-cpu-priority test-recording-cpu-priority.cpp ../utility/RecordingCpuPriority.cpp)
target_include_directories(test-recording-cpu-priority PRIVATE ..)
target_link_libraries(test-recording-cpu-priority PRIVATE OBS::libobs Qt::Core Qt::DBus)

# A private bus keeps the deterministic tests independent of the host's user
# manager. The opt-in --systemd test uses a real, self-only transient scope.
find_program(DBUS_RUN_SESSION_EXECUTABLE dbus-run-session)
if(DBUS_RUN_SESSION_EXECUTABLE)
  add_test(
    NAME recording-cpu-priority
    COMMAND "${DBUS_RUN_SESSION_EXECUTABLE}" -- $<TARGET_FILE:test-recording-cpu-priority>
  )
  set_tests_properties(recording-cpu-priority PROPERTIES TIMEOUT 30)
endif()
