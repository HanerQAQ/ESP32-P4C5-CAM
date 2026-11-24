set(srcs            object_detect_i2c_protocol.cpp)

set(include_dirs    .)

set(requires who_detect who_communicate espressif__cjson bsp driver)

idf_component_register(SRCS ${srcs}
                       INCLUDE_DIRS ${include_dirs}
                       REQUIRES ${requires})
