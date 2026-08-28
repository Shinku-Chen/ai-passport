idf_component_register(
    SRC_DIRS "."
    INCLUDE_DIRS "include"
    PRIV_REQUIRES
        esp_http_server
        esp_http_client
        esp_timer
        esp_netif
        lwip
    EMBED_FILES
        "xml/device.xml"
        "xml/avtransport.xml"
        "xml/renderingcontrol.xml"
        "xml/connectionmanager.xml"
)
