set(srcs            app_main.cpp
                    color_line_i2c_protocol.cpp
                    frame_cap_pipeline.cpp
                    line_regression_detect.cpp
                    vision_host_protocol.cpp)

set(include_dirs    ./)

set(requires who_detect_app who_communicate_app who_communicate color_detect who_web espressif__cjson nvs_flash)
set(detect_component ${DETECT_MODEL} espressif__${DETECT_MODEL})

idf_component_register(SRCS ${srcs} INCLUDE_DIRS ${include_dirs} REQUIRES ${requires})
idf_component_optional_requires(PUBLIC ${detect_component})
