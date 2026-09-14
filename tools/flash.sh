#!/usr/bin/env bash
# ESP32-S3-Touch-LCD-7Bへの書き込み(エージェントが実行する入口)。
#
# ビルド → 転送物のステージング → USB/IPのデタッチ → Windows側での書き込み →
# リセット → シリアルログの取得、までを通しで行う。実際の書き込みとリセットは
# Windows側の.devcontainer/win-agent.ps1が行い、ログは1行ずつlogs/へ流れてくる。
#
# 前提: Windows側でwin-agent.ps1が起動していること。
#       起動していない場合はtools/win.shが10秒でタイムアウトして失敗する。
#
# 使い方:
#   tools/flash.sh                          # ビルドから書き込み、その後180秒ログを取る
#   tools/flash.sh --no-build               # ビルド済みの成果物で書き込む
#   tools/flash.sh --port COM21 --baud 460800 --monitor 0
set -uo pipefail

PROJ="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

PORT="COM21"         # ESP32-S3-Touch-LCD-7BのネイティブUSBポート(USB-Serial/JTAG、
                     # VID:PID=303A:1001)。2026-09-14の実測値。CH343側(USB TO UART)は
                     # 別ポートに出るが、そちらからはROMブートローダに到達できない。
                     # 番号は挿し直しやPC構成の変更で変わる(COM4だった時期がある)。
                     # bash tools/win.sh portsで確認して--portで渡すか、ここを書き換える。
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
    echo "[1/6] ビルド"
    # shellcheck disable=SC1091
    . /opt/esp/idf/export.sh > /dev/null 2>&1
    if ! (cd "$PROJ" && idf.py build > "${PROJ}/logs/build.log" 2>&1); then
        echo "エラー: ビルドに失敗した。logs/build.logを参照すること。" >&2
        tail -20 "${PROJ}/logs/build.log" >&2
        exit 1
    fi
fi

# --- 2. 転送物のステージング --------------------------------------------------
echo "[2/6] 転送物のステージング"
if ! bash "${PROJ}/tools/stage-winflash.sh" > /dev/null; then
    echo "エラー: stage-winflash.shに失敗した。" >&2
    exit 1
fi

# --- 3. USB/IPのデタッチ ------------------------------------------------------
# アタッチされたままだとWindowsがCOMポートを開けない。外れていても失敗させない。
if [ -e /dev/ttyUSB0 ]; then
    echo "[3/6] USB/IPのデタッチ"
    "${PROJ}/.devcontainer/usb-attach.sh" detach || true
fi

# --- 4. Windows側へ転送 -------------------------------------------------------
echo "[4/6] 転送物の取得(Windows側)"
if ! WIN_TIMEOUT=180 bash "${PROJ}/tools/win.sh" sync; then
    echo "エラー: 転送物の取得に失敗した。" >&2
    exit 1
fi

# --- 5. 書き込み ---------------------------------------------------------------
echo "[5/6] 書き込み(port=${PORT} baud=${BAUD})"
if ! WIN_TIMEOUT=600 bash "${PROJ}/tools/win.sh" flash "port=${PORT}" "baud=${BAUD}"; then
    echo "エラー: 書き込みに失敗した。logs/flash.logを参照すること。" >&2
    tail -20 "${PROJ}/logs/flash.log" 2> /dev/null
    exit 1
fi
tail -5 "${PROJ}/logs/flash.log" 2> /dev/null

# --- 6. リセット ---------------------------------------------------------------
# --afterのhard_resetだけではアプリが起動しないことがあるため、書き込み後に
# 明示的なリセット(esptool run)を発行する。失敗してもログ取得は続ける。
echo "[6/6] リセット(port=${PORT})"
if ! WIN_TIMEOUT=30 bash "${PROJ}/tools/win.sh" reset "port=${PORT}" > /dev/null; then
    echo "警告: リセットの発行に失敗した(手動でのリセットが必要な場合がある)。"
fi

# --- シリアルログ ---------------------------------------------------------------
if [ "$MONITOR" != "0" ]; then
    echo "ログを${MONITOR}秒取得する"
    WIN_TIMEOUT=$((MONITOR + 60)) bash "${PROJ}/tools/win.sh" monitor "port=${PORT}" "sec=${MONITOR}" > /dev/null
    tail -40 "${PROJ}/logs/monitor.log" 2> /dev/null
fi

echo "完了。logs/flash.log と logs/monitor.log を参照。"
