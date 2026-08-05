# == DO NOT EDIT THE FOLLOWING LINES for the Raspberry Pi Pico VS Code Extension to work ==
if(WIN32)
    set(USERHOME $ENV{USERPROFILE})
else()
    set(USERHOME $ENV{HOME})
endif()
set(sdkVersion 2.2.0)
set(toolchainVersion 14_2_Rel1)
set(picotoolVersion 2.2.0-a4)
set(picoVscode ${USERHOME}/.pico-sdk/cmake/pico-vscode.cmake)
if (EXISTS ${picoVscode})
    include(${picoVscode})
endif()
# ====================================================================================
set(PICO_BOARD pico CACHE STRING "Board type")

cmake_minimum_required(VERSION 3.12)
include(pico_sdk_import.cmake)

project(2PORTmonitor C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

add_executable(2PORTmonitor
    2PortMonitor.c
    usb_descriptors.c
)

target_include_directories(2PORTmonitor PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}
)

target_link_libraries(2PORTmonitor
    pico_stdlib
    hardware_uart
    tinyusb_device
    tinyusb_board
)

# This project owns both TinyUSB CDC interfaces directly.
pico_enable_stdio_usb(2PORTmonitor 0)
pico_enable_stdio_uart(2PORTmonitor 0)

pico_add_extra_outputs(2PORTmonitor)
