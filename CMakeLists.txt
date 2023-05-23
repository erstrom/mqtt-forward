cmake_minimum_required(VERSION 3.5)

set(TARGET_MQTT_FORWARD mqtt-forward)

set(include_paths
    ${CMAKE_INSTALL_PREFIX}/include/
    ${CMAKE_INSTALL_PREFIX}/usr/include/
    ${CMAKE_CURRENT_LIST_DIR}/dep-install/include/
    ${CMAKE_CURRENT_SOURCE_DIR}/src/
    )

set(extra_link_directories
    )

set(source_files_mqtt_forward
    ${CMAKE_CURRENT_LIST_DIR}/mqtt-forward.c
    )

set(CFLAGS ${CFLAGS} -O0 -g3 -Wall)

add_compile_options(${CFLAGS})

project(mqtt-forward C)

link_directories(${CMAKE_INSTALL_PREFIX}/lib
                 ${CMAKE_INSTALL_PREFIX}/usr/lib
                 ${OPENSSL_ROOT_DIR}
                 ${extra_link_directories})

add_executable(${TARGET_MQTT_FORWARD} ${source_files_mqtt_forward})
target_include_directories(${TARGET_MQTT_FORWARD} PRIVATE ${include_paths})
target_link_libraries(${TARGET_MQTT_FORWARD} pthread mosquitto ssl crypto)

install(TARGETS "${TARGET_MQTT_FORWARD}"
		DESTINATION bin
	   )

# vim: set ts=4 sw=4 sts=4 et smarttab :
