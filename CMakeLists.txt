cmake_minimum_required(VERSION 3.5)
project(log_view)

find_package(PkgConfig REQUIRED)
pkg_check_modules(SYSTEMD REQUIRED libsystemd)

set(CURSES_NEED_NCURSES TRUE)
find_package(Curses REQUIRED)
find_library(CURSES_PANEL_LIBRARY NAMES panel)

###########
## Build ##
###########

include_directories(
  include
  ${CURSES_INCLUDE_DIRS}
  ${SYSTEMD_INCLUDE_DIRS}
)

add_library(${PROJECT_NAME}
  src/journal_reader.cpp
  src/log_filter.cpp
  src/log_store.cpp
  src/log_view.cpp
  src/panel_interface.cpp
  src/utils.cpp
  src/panels/details_panel.cpp
  src/panels/exclude_panel.cpp
  src/panels/filter_panel.cpp
  src/panels/help_panel.cpp
  src/panels/level_panel.cpp
  src/panels/log_panel.cpp
  src/panels/node_panel.cpp
  src/panels/search_panel.cpp
  src/panels/status_panel.cpp
)
target_link_libraries(${PROJECT_NAME}
  ${CURSES_LIBRARIES}
  ${CURSES_PANEL_LIBRARY}
  ${SYSTEMD_LIBRARIES}
)

add_executable(log_viewer src/main.cpp)
target_link_libraries(log_viewer ${PROJECT_NAME})

#############
## Install ##
#############

install(TARGETS ${PROJECT_NAME} log_viewer
  ARCHIVE DESTINATION lib
  LIBRARY DESTINATION lib
  RUNTIME DESTINATION bin
)
