cmake_minimum_required(VERSION 3.16)

# ホスト(ESP-IDFのlinuxターゲット)で走らせる単体テスト。実機もフラッシュも要らない。
# 対象は外部依存の無いロジック(schedule / text_util / time_util / json_parser)だけ。
# 実行はリポジトリ直下のtools/run-host-tests.sh。
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(host_test)
