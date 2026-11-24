set(srcs            app_main.cpp
                    frame_cap_pipeline.cpp
                    hand_gesture_i2c_protocol.cpp)

set(include_dirs    ./)

set(requires who_recognition_app who_communicate who_web espressif__cjson)
set(detect_component ${DETECT_MODEL} espressif__${DETECT_MODEL})

idf_component_register(SRCS ${srcs} INCLUDE_DIRS ${include_dirs} REQUIRES ${requires})
idf_component_optional_requires(PUBLIC ${detect_component})
