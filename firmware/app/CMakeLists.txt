cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(nrfnode)

target_sources(app PRIVATE
  src/main.cpp
  src/button_handler.cpp
  src/rtc_clock.cpp
  src/alarm.cpp
  src/storage.cpp
  src/devices.cpp
)

target_include_directories(app PRIVATE src)
