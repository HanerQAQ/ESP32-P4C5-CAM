set(src_dirs        ./)

set(include_dirs    ./)

set(requires who_detect_app who_communicate object_detect_i2c_protocol who_web espressif__cjson)
set(detect_component ${DETECT_MODEL} espressif__${DETECT_MODEL})

idf_component_register(SRC_DIRS ${src_dirs} INCLUDE_DIRS ${include_dirs} REQUIRES ${requires})
idf_component_optional_requires(PUBLIC ${detect_component})
