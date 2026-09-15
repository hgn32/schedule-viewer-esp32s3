# テスト対象はアプリのソースをそのままコンパイルする(コピーを作らない)。
set(APP_SRC_DIR ${CMAKE_CURRENT_LIST_DIR}/../../../main)

idf_component_register(
    SRCS "test_main.cpp"
         "test_text_util.cpp"
         "test_time_util.cpp"
         "test_schedule.cpp"
         "test_json_parser.cpp"
         "${APP_SRC_DIR}/text_util.cpp"
         "${APP_SRC_DIR}/schedule.cpp"
         "${APP_SRC_DIR}/json_parser.cpp"
    INCLUDE_DIRS "." "${APP_SRC_DIR}"
    REQUIRES unity json)
