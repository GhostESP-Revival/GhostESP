set(srcs "main.c" "station_example.c")

idf_component_register(SRCS "${srcs}"
                       INCLUDE_DIRS ".")
