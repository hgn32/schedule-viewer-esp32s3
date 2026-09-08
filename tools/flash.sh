#!/usr/bin/env bash
# ESP32-S3-Touch-LCD-7Bへの書き込み(エージェントが実行する入口)。
#
# ビルド → 転送物のステージング → USB/IPのデタッチ → Windows側での書き込み →
# シリアルログの取得、までを通しで行う。実際の書き込みはWindows側の
# .devcontainer/win-agent.ps1が行い、ログは1行ずつlogs/へ流れてくる。
#
# 前提: Windows側でwin-agent.ps1が起動していること。
#       起動していない場合はtools/win.shが10秒でタイムアウトして失敗する。
#
# 使い方:
#   tools/flash.sh                          # ビルドから書き込み、その後180秒ログを取る
#   tools/flash.sh --no-build               # ビルド済みの成果物で書き込む
#   tools/flash.sh --port COM4 --baud 460800 --monitor 0
set -uo pipefail

PROJ="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

PORT="COM11"        # M5Paper時代の実測値。ESP32-S3-Touch-LCD-7Bでは未確定。
                     # bash tools/win.sh ports で確認して --port で渡すか、ここを書き換える。
BAUD="921600"
MONITOR="180"
DO_BUILD=1

while [ $# -gt 0 ]; do
    case "$1" in
        --port)     PORT="$2"; shift 2 ;;
        --baud)     BAUD="$2"; shift 2 ;;
        --monitor)  MONITOR="$2"; shift 2 ;;
        --no-build) DO_BUILD=0; shift ;;
        *) echo "不明な引数: $1" >&2; exit 2 ;;
    esac
done

mkdir -p "${PROJ}/logs"

# --- 1. ビルド ---------------------------------------------------------------
if [ "$DO_BUILD" = "1" ]; then
    echo "[1/5] ビルド"
    # shellcheck disable=SC1091
    . /opt/esp/idf/export.sh > /dev/null 2>&1
    if ! (cd "$PROJ" && idf.py build > "${PROJ}/logs/build.log" 2>&1); then
        echo "エラー: ビルドに失敗した。logs/build.logを参照すること。" >&2
        tail -20 "${PROJ}/logs/build.log" >&2
        exit 1
    fi
fi

# --- 2. 転送物のステージング --------------------------------------------------
echo "[2/5] 転送物のステージング"
if ! bash "${PROJ}/tools/stage-winflash.sh" > /dev/null; then
    echo "エラー: stage-winflash.shに失敗した。" >&2
    exit 1
fi

# --- 3. USB/IPのデタッチ ------------------------------------------------------
# アタッチされたままだとWindowsがCOMポートを開けない。外れていても失敗させない。
if [ -e /dev/ttyUSB0 ]; then
    echo "[3/5] USB/IPのデタッチ"
    "${PROJ}/.devcontainer/usb-attach.sh" detach || true
fi

# --- 4. Windows側へ転送して書き込む -------------------------------------------
echo "[4/5] 転送物の取得(Windows側)"
if ! WIN_TIMEOUT=180 bash "${PROJ}/tools/win.sh" sync; then
    echo "エラー: 転送物の取得に失敗した。" >&2
    exit 1
fi

echo "[5/5] 書き込み(port=${PORT} baud=${BAUD})"
if ! WIN_TIMEOUT=600 bash "${PROJ}/tools/win.sh" flash "port=${PORT}" "baud=${BAUD}"; then
    echo "エラー: 書き込みに失敗した。logs/flash.logを参照すること。" >&2
    tail -20 "${PROJ}/logs/flash.log" 2> /dev/null
    exit 1
fi
tail -5 "${PROJ}/logs/flash.log" 2> /dev/null

# --- 5. シリアルログ ----------------------------------------------------------
if [ "$MONITOR" != "0" ]; then
    echo "ログを${MONITOR}秒取得する"
    WIN_TIMEOUT=$((MONITOR + 60)) bash "${PROJ}/tools/win.sh" monitor "port=${PORT}" "sec=${MONITOR}" > /dev/null
    tail -40 "${PROJ}/logs/monitor.log" 2> /dev/null
fi

echo "完了。logs/flash.log と logs/monitor.log を参照。"
