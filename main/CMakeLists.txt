idf_component_register(
    SRCS "main.cc" "boards/common/rndis_board.cc"
    PRIV_INCLUDE_DIRS "boards/common"
    PRIV_REQUIRES esp_event esp_netif esp_system iot_usbh_cdc iot_usbh_rndis
)
