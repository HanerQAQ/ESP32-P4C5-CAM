set(srcs            who_color_adjust_task.cpp
                    who_ci1302_task.cpp
                    who_protocol.cpp
                    who_i2c_slave_task.cpp
                    who_speech_recognition_store.cpp)

set(include_dirs    .)

set(requires who_task who_frame_cap bsp driver esp_timer heap esp_ringbuf esp_driver_usb_serial_jtag espressif__esp_new_jpeg espressif__cjson)

idf_component_register(SRCS ${srcs}
                       INCLUDE_DIRS ${include_dirs}
                       REQUIRES ${requires})
