cmake_minimum_required(VERSION 3.16)
project(Monk_SKUD_UNEX VERSION 0.3.44 LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
find_package(OpenSSL REQUIRED)
find_package(Threads REQUIRED)
find_path(MARIADB_INCLUDE_DIR mariadb/mysql.h REQUIRED)
find_library(MARIADB_LIBRARY NAMES mariadb REQUIRED)

# Embed the current web UI into the executable. RuntimeBootstrap writes these
# files into an empty working directory on the first start.
function(skud_embed_file input_file output_var)
  file(READ "${input_file}" _skud_hex HEX)
  string(REGEX REPLACE "([0-9A-Fa-f][0-9A-Fa-f])" "0x\\1," _skud_bytes "${_skud_hex}")
  set(${output_var} "${_skud_bytes}" PARENT_SCOPE)
endfunction()

skud_embed_file("${CMAKE_SOURCE_DIR}/web/login.html" EMBED_LOGIN_HTML)
skud_embed_file("${CMAKE_SOURCE_DIR}/web/index.html" EMBED_INDEX_HTML)
skud_embed_file("${CMAKE_SOURCE_DIR}/web/app.js" EMBED_APP_JS)
skud_embed_file("${CMAKE_SOURCE_DIR}/web/style.css" EMBED_STYLE_CSS)
skud_embed_file("${CMAKE_SOURCE_DIR}/web/monitor.html" EMBED_MONITOR_HTML)
skud_embed_file("${CMAKE_SOURCE_DIR}/web/monitor.js" EMBED_MONITOR_JS)
skud_embed_file("${CMAKE_SOURCE_DIR}/web/monitor.css" EMBED_MONITOR_CSS)
skud_embed_file("${CMAKE_SOURCE_DIR}/web/mag250.html" EMBED_MAG250_HTML)
skud_embed_file("${CMAKE_SOURCE_DIR}/web/mag250.js" EMBED_MAG250_JS)
skud_embed_file("${CMAKE_SOURCE_DIR}/web/mag250.css" EMBED_MAG250_CSS)
file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/generated/skud")
configure_file(
  "${CMAKE_SOURCE_DIR}/cmake/EmbeddedWeb.h.in"
  "${CMAKE_BINARY_DIR}/generated/skud/EmbeddedWeb.h"
  @ONLY
)
add_executable(monk-skud-unex
  src/main.cpp
  src/app/App.cpp
  src/config/Config.cpp
  src/utils/Util.cpp
  src/system/SystemMetrics.cpp
  src/storage/FileStore.cpp
  src/storage/MariaDbUserStore.cpp
  src/runtime/RuntimeBootstrap.cpp
  src/users/UserManager.cpp
  src/import/SoyalImport.cpp
  src/departments/DepartmentManager.cpp
  src/attendance/AttendanceEngine.cpp
  src/serial/SerialPort.cpp
  src/unex/Unex721Protocol.cpp
  src/controllers/ControllerManager.cpp
  src/telegram/TelegramNotifier.cpp
  src/reports/ReportManager.cpp
  src/web/WebServer.cpp
)
target_include_directories(monk-skud-unex PRIVATE include "${CMAKE_BINARY_DIR}/generated" "${MARIADB_INCLUDE_DIR}")
target_link_libraries(monk-skud-unex PRIVATE OpenSSL::Crypto Threads::Threads "${MARIADB_LIBRARY}")
target_compile_options(monk-skud-unex PRIVATE -Wall -Wextra -Wpedantic)

# Keep the web UI next to the binary as well as in the source tree.
# This lets the application serve static files even when started from build/.
file(COPY "${CMAKE_SOURCE_DIR}/web" DESTINATION "${CMAKE_BINARY_DIR}")
install(TARGETS monk-skud-unex RUNTIME DESTINATION bin)
