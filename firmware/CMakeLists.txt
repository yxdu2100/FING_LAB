cmake_minimum_required(VERSION 3.20.0)
# Keep Zephyr's generated compiler-capability cache inside the selected build
# directory. This also makes command-line builds independent of host cache access.
set(USER_CACHE_DIR "${CMAKE_BINARY_DIR}/.cache" CACHE PATH "Zephyr user cache")
list(APPEND BOARD_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/..)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})

project(fing_lab_capture)

target_sources(app PRIVATE
    src/main.c
    src/ble/ble.c
    src/modes/mode_manager.c
    src/modules/camera/camera.c
    src/modules/imu/imu.c
    src/modules/imu/imu_stream.c
    src/modules/tof/tof.c
)

target_include_directories(app PRIVATE
    src
    src/ble
    src/modes
    src/modules/camera
    src/modules/imu
    src/modules/tof
)
