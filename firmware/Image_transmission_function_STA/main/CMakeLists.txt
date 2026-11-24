set(srcs "app_main.c"
         "camera_service.c"
         "network_service.c"
         "app_mywifi.c"
         "dns_server.c"
         "app_myi2c.c"
         "app_myperiph.c"
)
set(html_files "../frontend/gzipped/index.html.gz"
               "../frontend/gzipped/loading.jpg.gz"
               "../frontend/gzipped/favicon.ico.gz"
               "../frontend/gzipped/assets/index.js.gz"
               "../frontend/gzipped/assets/index.css.gz")

idf_component_register(SRCS "${srcs}"
                       PRIV_INCLUDE_DIRS .
                       EMBED_TXTFILES ${html_files})
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-error -w)
