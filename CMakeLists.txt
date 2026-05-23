zephyr_library()
zephyr_include_directories(${APPLICATION_SOURCE_DIR}/include)

zephyr_library_sources_ifdef(CONFIG_CORNE_BLE_DEBUG src/corne_ble_debug.c)
