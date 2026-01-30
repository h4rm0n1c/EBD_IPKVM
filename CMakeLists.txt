cmake_minimum_required(VERSION 3.13)

# Pull in the Pico SDK (must have PICO_SDK_PATH set in the environment)
include($ENV{PICO_SDK_PATH}/external/pico_sdk_import.cmake)

project(EBD_IPKVM C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

add_executable(EBD_IPKVM
    src/main.c
    src/video_capture.c
)

pico_generate_pio_header(EBD_IPKVM ${CMAKE_CURRENT_LIST_DIR}/src/classic_line.pio)

pico_enable_stdio_usb(EBD_IPKVM 1)
pico_enable_stdio_uart(EBD_IPKVM 0)

target_compile_options(EBD_IPKVM PRIVATE -O3)

target_link_libraries(EBD_IPKVM
    pico_stdlib
    pico_multicore
    hardware_pio
    hardware_dma
    hardware_irq
)

pico_add_extra_outputs(EBD_IPKVM)
