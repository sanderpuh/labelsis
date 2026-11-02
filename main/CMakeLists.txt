idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    PRIV_INCLUDE_DIRS "${BOARD_DIR}"
    REQUIRES pt_app
)
