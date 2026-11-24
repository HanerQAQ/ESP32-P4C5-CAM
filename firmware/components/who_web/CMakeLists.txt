set(src_dirs .)

set(include_dirs .)

set(requires who_frame_cap
             who_task
             esp_http_server
             esp_netif
             esp_event
             esp_wifi
             nvs_flash
             esp_timer
             esp_driver_jpeg
             esp_driver_ppa
             espressif__esp_new_jpeg
             espressif__cjson)

set(html_files "frontend/gzipped/index.html.gz"
               "frontend/gzipped/loading.jpg.gz"
               "frontend/gzipped/favicon.ico.gz"
               "frontend/gzipped/assets/index.js.gz"
               "frontend/gzipped/assets/index.css.gz")

idf_component_register(SRC_DIRS ${src_dirs}
                       INCLUDE_DIRS ${include_dirs}
                       REQUIRES ${requires}
                       EMBED_TXTFILES ${html_files})

target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-error)
