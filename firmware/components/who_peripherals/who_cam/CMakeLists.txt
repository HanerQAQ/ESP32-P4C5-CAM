set(src_dirs        .)

set(include_dirs    .)

set(requires esp_timer esp-dl esp_lcd esp_video bsp)

idf_component_register(SRC_DIRS ${src_dirs} INCLUDE_DIRS ${include_dirs} REQUIRES ${requires})
