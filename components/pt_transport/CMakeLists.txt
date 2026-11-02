# QL-700 USB host transport. The ESP-IDF USB host component is supplied
# by the PlatformIO Espressif32 platform.
idf_component_register(
    SRCS "src/usb_host.c"
    INCLUDE_DIRS "include"
    REQUIRES usb
)
