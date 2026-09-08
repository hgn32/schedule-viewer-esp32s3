#!/usr/bin/env bash
# devcontainerの初回作成時に一度だけ走る。
# 失敗してもコンテナ作成は止めない(最後に必ずexit 0する)。
set -u

. /opt/esp/idf/export.sh >/dev/null 2>&1 || {
    echo "[post-create] export.shの読み込みに失敗した。idf.pyは手動で有効化すること。"
    exit 0
}

cd /workspaces || exit 0

if [ -f sdkconfig ]; then
    echo "[post-create] sdkconfigは既にある。set-targetはスキップする。"
    exit 0
fi

echo "[post-create] idf.py set-target esp32s3 を実行する(managed_components/も取得される)。"
idf.py set-target esp32s3 || echo "[post-create] set-targetに失敗した。コンテナ内で手動実行すること。"

exit 0
