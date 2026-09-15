#!/usr/bin/env bash
# ホスト(ESP-IDFのlinuxターゲット)で単体テストを走らせる。実機もフラッシュも要らない。
#
# 対象はLCD・Wi-Fi・IDFのハードウェア層に依存しないロジックだけ:
#   main/text_util.cpp   件名・場所・本文の正規化
#   main/time_util.h     ISO8601のパースと表示用の整形
#   main/schedule.cpp    予定の保持と期間フィルタ
#   main/json_parser.cpp サーバのレスポンスからEventへの変換(除外条件を含む)
#
# テスト本体はtest/host/main/。アプリのソースはコピーせず、そのままコンパイルする。
set -uo pipefail

PROJ="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TEST_DIR="${PROJ}/test/host"

# 非対話シェルはPATHにidf.pyが無いのでexport.shの読み込みが必須。
. /opt/esp/idf/export.sh > /dev/null 2>&1

cd "${TEST_DIR}" || exit 1

# linuxターゲットはESP-IDFではプレビュー扱い。初回と、実機用のターゲットが
# 残っているときだけ設定し直す(実機側のsdkconfigとは別ディレクトリなので干渉しない)。
if ! grep -q 'CONFIG_IDF_TARGET="linux"' sdkconfig 2> /dev/null; then
    echo "[1/3] linuxターゲットを設定する"
    if ! idf.py --preview set-target linux > /dev/null; then
        echo "エラー: linuxターゲットの設定に失敗した。" >&2
        exit 1
    fi
fi

echo "[2/3] ビルド"
if ! idf.py build > /dev/null; then
    echo "エラー: テストのビルドに失敗した。idf.py buildを直接実行して原因を見ること。" >&2
    exit 1
fi

echo "[3/3] 実行"
./build/host_test.elf
