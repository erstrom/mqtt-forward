cmake_minimum_required(VERSION 3.5)
project(mqtt-forward C)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type (default Debug)" FORCE)
endif()

set(TARGET_MQTT_FORWARD mqtt-forward)

set(source_files_mqtt_forward
    src/beacon.c
    src/mqtt-forward.c
    src/protocol.c
    src/session.c
    src/utils.c
)

add_executable(${TARGET_MQTT_FORWARD} ${source_files_mqtt_forward})

target_include_directories(${TARGET_MQTT_FORWARD} PRIVATE
    ${CMAKE_INSTALL_PREFIX}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# Optional: Add compile options (if not handled by flags)
target_compile_options(${TARGET_MQTT_FORWARD} PRIVATE -Wall)

# Optional: Override CMake defaults if needed
set(CMAKE_C_FLAGS_DEBUG "-O0 -g3")
set(CMAKE_C_FLAGS_RELEASE "-O3 -DNDEBUG")

find_package(OpenSSL REQUIRED)
target_link_libraries(${TARGET_MQTT_FORWARD} PRIVATE
    pthread
    mosquitto
    OpenSSL::SSL
    OpenSSL::Crypto
)

install(TARGETS ${TARGET_MQTT_FORWARD} DESTINATION bin)

# vim: set ts=4 sw=4 sts=4 et smarttab :
