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

#Set the mimum required version of CMake
cmake_minimum_required(VERSION 3.12)

#Pull in SDK (must be before project)
include(pico_sdk_import.cmake)

#Set the project name, the programming language
project(project1 C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

#Initialize the SDK
pico_sdk_init()

#Add an executable target to be build from the source files
add_executable(2PORTmonitor 2PortMonitor.c)

pico_generate_pio_header(2PORTmonitor ${CMAKE_CURRENT_LIST_DIR}/ws2812.pio OUTPUT_DIR ${CMAKE_CURRENT_LIST_DIR}/generated)

#Pull in our pico_stdlib which aggregates commonly used features
target_link_libraries(2PORTmonitor pico_stdlib hardware_adc hardware_pll hardware_clocks)

#Enable USB output, disable UART output
pico_enable_stdio_usb(2PORTmonitor 1)
pico_enable_stdio_uart(2PORTmonitor 0)

# Add any user requested libraries
target_link_libraries(2PORTmonitor
    pico_stdlib
    hardware_pio
    hardware_spi
    hardware_adc
    hardware_pll
)

#Create map/bin/hex/uf2 file etc.
pico_add_extra_outputs(2PORTmonitor)
