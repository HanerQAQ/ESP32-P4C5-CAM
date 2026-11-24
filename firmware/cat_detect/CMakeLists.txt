cmake_minimum_required(VERSION 3.5)

set(EXTRA_COMPONENT_DIRS ../components/who_task
                         ../components/who_peripherals/who_cam
                         ../components/who_peripherals/who_lcd
                         ../components/who_frame_cap
                         ../components/who_frame_lcd_disp
                         ../components/who_detect
                         ../components/who_communicate
                         ../components/object_detect_i2c_protocol
                         ../components/bsp
                         ../components/who_app/who_detect_app
                         ../components/who_web)

add_compile_options(-fdiagnostics-color=always)
# GCC 15 / Picolibc with gnu++26 requires explicit <cmath> inclusion in some
# managed components (notably esp-dsp 1.7.0). Apply it only to C++ sources.
add_compile_options($<$<COMPILE_LANGUAGE:CXX>:-include> $<$<COMPILE_LANGUAGE:CXX>:cmath>)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(cat_detect)
