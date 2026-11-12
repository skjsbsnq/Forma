cmake_minimum_required(VERSION 3.21)

# Verify a login transaction offline: never load or restart the user's services.
# SYSTEMD_UNIT_PATH excludes local overrides and installed Plasma versions.
file(READ "${AGENT_UNIT}" agent)
# The binary need not be installed for unit ordering tests.
string(REPLACE "ExecStart=/usr/bin/forma-polkit-agent" "ExecStart=/usr/bin/true" agent "${agent}")

foreach(desktop IN ITEMS plasma generic)
  set(units "${VERIFY_DIR}/${desktop}")
  file(MAKE_DIRECTORY "${units}")
  file(WRITE "${units}/forma-polkit-agent.service" "${agent}")
  foreach(target IN ITEMS basic shutdown graphical-session-pre)
    file(WRITE "${units}/${target}.target" "[Unit]\nDescription=Test ${target}\n")
  endforeach()
  # enable creates this Wants relation on every supported desktop.
  file(WRITE "${units}/graphical-session.target"
    "[Unit]\nDescription=Test graphical session\nWants=forma-polkit-agent.service\n")
  if(desktop STREQUAL "plasma")
    file(CREATE_LINK "forma-polkit-agent.service" "${units}/plasma-polkit-agent.service" SYMBOLIC)
    # Plasma pulls in the alias before graphical-session.target. Ordering the
    # agent after that target makes a cycle, even if standalone verify passes.
    file(WRITE "${units}/plasma-workspace.target"
      "[Unit]\nDescription=Test Plasma workspace\nRequires=plasma-core.target graphical-session.target\nWants=plasma-polkit-agent.service\nBefore=graphical-session.target\n")
    file(WRITE "${units}/plasma-core.target"
      "[Unit]\nDescription=Test Plasma core\nWants=graphical-session-pre.target plasma-kwin_wayland.service\nAfter=graphical-session-pre.target plasma-kwin_wayland.service\n")
    file(WRITE "${units}/plasma-kwin_wayland.service"
      "[Unit]\nDescription=Test compositor\n[Service]\nExecStart=/usr/bin/true\n")
    set(root plasma-workspace.target)
  else()
    # Non-Plasma desktops must not pull in a KDE service merely for ordering.
    set(root graphical-session.target)
  endif()
  execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env "SYSTEMD_UNIT_PATH=${units}" "SYSTEMD_LOG_LEVEL=warning"
      "${SYSTEMD_ANALYZE}" --user --man=no --generators=no verify "${units}/${root}"
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
  if(NOT result EQUAL 0 OR "${output}${error}" MATCHES "[Cc]ycle|[Oo]rdering")
    message(FATAL_ERROR "${desktop} login transaction failed (${result}):\n${output}${error}")
  endif()
endforeach()

# A cycle-free unit can still start too early. Plasma imports its display
# environment before core.target; pre.target alone does not wait for KWin.
file(STRINGS "${AGENT_UNIT}" afterLines REGEX "^After=")
set(after)
foreach(line IN LISTS afterLines)
  string(REGEX REPLACE "^After=" "" line "${line}")
  separate_arguments(dependencies UNIX_COMMAND "${line}")
  list(APPEND after ${dependencies})
endforeach()
foreach(required IN ITEMS graphical-session-pre.target plasma-core.target)
  if(NOT required IN_LIST after)
    message(FATAL_ERROR "Agent must wait for ${required} before inheriting the display environment")
  endif()
endforeach()
message(STATUS "Authentication service ordering passed (Plasma and generic sessions)")
