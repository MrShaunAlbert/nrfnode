cmake_minimum_required(VERSION 3.20.0)

# Matter-modulen bygger libCHIP utan controller på Zephyr. Flaggan måste sättas innan
# find_package(Zephyr) eftersom modulens CMake läser MATTER_GN_ARGS när den läses in.
string(APPEND MATTER_GN_ARGS "--arg\nchip_build_controller\ntrue\n")

find_package(Zephyr HINTS $ENV{ZEPHYR_BASE})

project(nrfnode-matter-proto)

include(${ZEPHYR_CONNECTEDHOMEIP_MODULE_DIR}/config/nrfconnect/app/enable-gnu-std.cmake)
include(${ZEPHYR_NRF_MODULE_DIR}/samples/matter/common/cmake/source_common.cmake)
include(${ZEPHYR_NRF_MODULE_DIR}/samples/matter/common/cmake/data_model.cmake)
include(${ZEPHYR_NRF_MODULE_DIR}/samples/matter/common/cmake/zap_helpers.cmake)
ncs_get_zap_parent_dir(ZAP_PARENT_DIR)

target_include_directories(app PRIVATE
  src
  ../app/src
  ${ZAP_PARENT_DIR}
)

target_compile_options(app PRIVATE -Wno-deprecated-declarations)
target_compile_definitions(app PRIVATE NRFNODE_MATTER_UI_THREAD)

target_sources(app PRIVATE
  src/app_task.cpp
  src/main.cpp
  src/controller.cpp
  src/ble_scan.cpp
  src/ble_central.cpp
  ../app/src/main.cpp
  ../app/src/button_handler.cpp
  ../app/src/rtc_clock.cpp
  ../app/src/alarm.cpp
  ../app/src/storage.cpp
  ../app/src/devices.cpp
)

ncs_configure_data_model()
