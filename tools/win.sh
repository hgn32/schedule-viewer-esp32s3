#!/usr/bin/env bash
# Windows側へ操作を依頼し、結果を受け取る(エージェントが使う入口)。
#
# 依頼できるのは.devcontainer/win-agent.ps1が持つ5つの操作だけ。
#
#   tools/win.sh sync                          転送物(esptool一式と.bin)をWindowsへ取得
#   tools/win.sh ports                         COMポートの一覧
#   tools/win.sh probe   port=COM3             chip_id(書き換えは起きない)
#   tools/win.sh reset   port=COM4             アプリを起動させる(リセットのみ)
#   tools/win.sh flash   port=COM3 baud=921600 書き込み
#   tools/win.sh monitor port=COM3 sec=180     シリアルログの取得
#   tools/win.sh restart                       win-agent.ps1を自己更新して再起動させる
#
# 前提: Windows側でwin-agent.ps1が起動していること。起動していなければ要求の送出が
#       10秒でタイムアウトして失敗する(黙って止まらない)。
#
# 環境変数:
#   WIN_TIMEOUT  結果を待つ上限秒数(既定300)
set -uo pipefail

PROJ="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
FIFO="${WIN_REQUEST_FIFO:-${PROJ}/.win-request}"
LOG_DIR="${PROJ}/logs/win"
TIMEOUT="${WIN_TIMEOUT:-300}"

if [ $# -lt 1 ]; then
    # 先頭のコメント塊(2行目以降)だけを使い方として出す。行数で切ると
    # コメントを増やしたときにコード行まで出てしまうため、最初の非コメント行で止める。
    awk 'NR>1 { if ($0 !~ /^#/) exit; sub(/^# ?/, ""); print }' "${BASH_SOURCE[0]}"
    exit 2
fi

ACTION="$1"
shift
ARGS="$*"

case "$ACTION" in
    sync|ports|probe|reset|flash|monitor|restart) ;;
    *) echo "エラー: 未知の操作 '${ACTION}'。sync/ports/probe/reset/flash/monitor/restartのいずれか。" >&2
       exit 2 ;;
esac

mkdir -p "$LOG_DIR"

if [ ! -p "$FIFO" ]; then
    echo "エラー: ${FIFO}が無い。Windows側でwin-agent.ps1が起動していない。" >&2
    exit 1
fi

ID="$(date +%Y%m%d-%H%M%S)-$$"
RESULT="${LOG_DIR}/${ID}.log"

# FIFOへ「ID<タブ>操作 引数」を書く。読み手が居なければ10秒で諦める。
if ! timeout 10 bash -c "printf '%s\t%s %s\n' '$ID' '$ACTION' '$ARGS' > '$FIFO'"; then
    echo "エラー: 要求を送れなかった(10秒でタイムアウト)。" >&2
    echo "Windows側のwin-agent.ps1が起動しているか確認すること。" >&2
    exit 1
fi

# restartは結果が返らない(Windows側が接続を切って再起動するため)。
if [ "$ACTION" = "restart" ]; then
    echo "再起動を指示した。数秒後に再接続される。"
    exit 0
fi

# 結果ファイルに終了マーカーが現れるまで待つ。
waited=0
while [ "$waited" -lt "$TIMEOUT" ]; do
    if [ -f "$RESULT" ] && grep -q '=== EXIT rc=' "$RESULT"; then
        rc=$(grep -o '=== EXIT rc=[0-9-]*' "$RESULT" | tail -n1 | sed 's/.*rc=//')
        grep -v '=== EXIT rc=' "$RESULT"
        exit "${rc:-1}"
    fi
    sleep 1
    waited=$((waited + 1))
done

echo "エラー: ${TIMEOUT}秒待ったが結果が返らなかった(${RESULT})。" >&2
echo "Windows側の画面とlogs/を確認すること。" >&2
exit 1
