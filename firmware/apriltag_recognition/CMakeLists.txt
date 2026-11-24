cmake_minimum_required(VERSION 3.5)

set(EXTRA_COMPONENT_DIRS ../components/who_task
                         ../components/who_peripherals/who_cam
                         ../components/who_peripherals/who_lcd
                         ../components/who_frame_cap
                         ../components/who_frame_lcd_disp
                         ../components/who_apriltag
                         ../components/who_communicate
                         ../components/who_app/who_apriltag_app
                         ../components/bsp
                         ../components/who_web)

add_compile_options(-fdiagnostics-color=always)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(apriltag_recognition)
