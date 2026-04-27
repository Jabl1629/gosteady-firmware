cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(gosteady_firmware LANGUAGES C)

target_sources(app PRIVATE
  src/main.c
  src/session.c
  src/dump.c
  src/control.c
  src/cellular.c
  src/cloud.c
  src/algo/gs_filters.c
  src/algo/gs_motion_gate.c
  src/algo/gs_step_detector.c
  src/algo/gs_roughness.c
  src/algo/gs_pipeline.c
)
target_include_directories(app PRIVATE src)
