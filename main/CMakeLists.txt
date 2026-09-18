idf_component_register(SRCS "main.c" "qrcodegen.c"
                    INCLUDE_DIRS "."
                    REQUIRES driver esp_lcd console vfs)
