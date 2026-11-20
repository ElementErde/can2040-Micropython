add_library(usermod_can2040 INTERFACE)
target_sources(usermod_can2040 INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}/modcan2040.c
    ${CMAKE_CURRENT_LIST_DIR}/can2040.c
)
target_include_directories(usermod_can2040 INTERFACE ${CMAKE_CURRENT_LIST_DIR})
target_link_libraries(usermod_can2040 INTERFACE
    hardware_pio hardware_irq hardware_gpio hardware_clocks cmsis_core)
target_link_libraries(usermod INTERFACE usermod_can2040)
