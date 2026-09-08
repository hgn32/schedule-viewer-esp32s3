#!/usr/bin/env bash
# Windows側の実行スクリプト(.devcontainer/win-agent.ps1)がsshで起動する待ち受け側。
#
# devcontainerからWindowsへ発信することはできないので、Windows側から張ったsshの
# 標準出力を指示の通り道として使う。このスクリプトはFIFOをブロック読みし、
# tools/win.shが書き込んだ1行をそのまま標準出力へ流す。その行がsshを逆流して
# Windows側へ届き、決められた操作が実行される。
#
#   コンテナ: tools/win.sh --> FIFO --> このスクリプト --(ssh stdout)--> Windows
#
# 待ち受けポートは増えない(既存のsshdだけを使う)。Windows側は接続する側のまま。
set -uo pipefail

FIFO="${WIN_REQUEST_FIFO:-/workspaces/.win-request}"

# Windows側が落ちて張り直すと、前回のsshで起動したこのスクリプトが孤児として残る。
# 読み手が複数あるとFIFOへ書いた要求が孤児に吸われ、結果が返らなくなる。
# 新しいインスタンスが起動した時点で古いものを終了させる。
for pid in $(pgrep -f '^bash /workspaces/tools/win-agent\.sh$' 2> /dev/null); do
    [ "$pid" = "$$" ] && continue
    [ "$pid" = "$PPID" ] && continue
    kill "$pid" 2> /dev/null || true
done

if [ -e "$FIFO" ] && [ ! -p "$FIFO" ]; then
    rm -f "$FIFO"
fi
if [ ! -p "$FIFO" ]; then
    mkfifo -m 600 "$FIFO"
fi

mkdir -p /workspaces/logs/win

# Windows側が接続の成立を確認するための1行。
printf 'AGENT READY %s\n' "$(date -Is)"

# 読み書き両方で開く。こうしないとFIFOのopenが書き込み側を待ってブロックし、
# readのタイムアウトが効かない。
exec 3<> "$FIFO"

while :; do
    # 30秒読めなければ生存確認の1行を出す。Windows側のps1が死んでsshだけが
    # 残っている場合、この書き込みが失敗してこのスクリプトも終了する。
    # 放置すると、取り残された中継が要求を読み取って握り潰してしまう。
    if read -t 30 -r line <&3; then
        [ -n "$line" ] || continue
        printf '%s\n' "$line" || exit 0
    else
        printf 'HEARTBEAT %s\n' "$(date -Is)" || exit 0
    fi
done
