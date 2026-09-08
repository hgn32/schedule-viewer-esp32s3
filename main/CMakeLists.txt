# main/*.cppをGLOBしているので、ソースを増やすときに追記は不要。
# ただしMINIMAL_BUILDが効いているため、新しいIDFコンポーネントに依存したら
# REQUIRESへの追加が必要(書かないコンポーネントはビルド対象に入らない)。
file(GLOB_RECURSE MAIN_SRCS ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp)

idf_component_register(
    SRCS ${MAIN_SRCS}
    INCLUDE_DIRS "."
    # esp_wifi以降はサーバから予定を取得するHTTPS経路のために必要。
    # mbedtlsはesp_crt_bundle.h(公的CAバンドル)の提供元。
    # esp_lcdはRGB LCDパネルの駆動、esp_partitionはfontパーティションのmmap、
    # esp_mmはesp_partition_mmapが内部で使うメモリマッピング、
    # esp_driver_i2cはIOエキスパンダ(バックライト/リセット)制御用。
    # freetypeとlovyangfxはコンポーネントマネージャが依存として自動で要求に加える。
    REQUIRES driver esp_timer
             esp_wifi esp_event esp_netif nvs_flash
             esp_http_client esp-tls mbedtls json
             esp_lcd esp_partition esp_mm esp_driver_i2c
)
