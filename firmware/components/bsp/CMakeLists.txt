set(include_dirs ".")

set(src_dirs ".")

set(requires esp_lcd esp_driver_i2c esp_driver_uart esp_driver_gpio)

idf_component_register(SRC_DIRS ${src_dirs} INCLUDE_DIRS ${include_dirs} REQUIRES ${requires})
