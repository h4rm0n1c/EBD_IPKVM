cmake_minimum_required(VERSION 3.13)

# Pull in the Pico SDK (must have PICO_SDK_PATH set in the environment)
include($ENV{PICO_SDK_PATH}/external/pico_sdk_import.cmake)

project(EBD_IPKVM C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

add_executable(EBD_IPKVM
    src/app_core.c
    src/core_bridge.c
    src/main.c
    src/usb_control.c
    src/usb_descriptors.c
    src/video_capture.c
    src/video_core.c
)

pico_generate_pio_header(EBD_IPKVM ${CMAKE_CURRENT_LIST_DIR}/src/classic_line.pio)

pico_enable_stdio_usb(EBD_IPKVM 0)
pico_enable_stdio_uart(EBD_IPKVM 0)

target_compile_options(EBD_IPKVM PRIVATE -O3)

target_link_libraries(EBD_IPKVM
    pico_stdlib
    pico_multicore
    pico_unique_id
    hardware_pio
    hardware_dma
    hardware_irq
    tinyusb_device
    tinyusb_board
)

target_include_directories(EBD_IPKVM PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/src
)

pico_add_extra_outputs(EBD_IPKVM)
