#!/usr/bin/env bash
# 基板のUSB-TO-UARTポート(ブリッジチップ名は未確認)をUSB/IP経由でこのコンテナに取り込むためのヘルパー。
#
# 経路(SSHリバースポートフォワード方式。構成図はREADMEの「実機接続(USB/IP)」節):
#   1. Windows: usbipd-winが基板のUSBシリアルを127.0.0.1:3240で共有する
#   2. VS Code: このコンテナのsshd(127.0.0.1:2222)をWindowsのlocalhostへ転送する
#      (devcontainer.jsonのforwardPorts)
#   3. Windows: `ssh -N devcontainer`でそのsshdへ繋ぎ、RemoteForwardで
#      コンテナの127.0.0.1:3240をWindowsの127.0.0.1:3240へ通す
#   4. コンテナ: usbip attach -r 127.0.0.1 -b <busid> でデバイスを取り込む
#
# devcontainerからWindows PCへ向かう通信はネットワークポリシー上許可されていないため、
# 代わりにWindows側から張るSSHにリバースポートフォワードを乗せ、通信の向きを
# 反転させている。usbipdはlocalhostからの接続だけ受ければよいので、Windows側の
# ファイアウォールでTCP 3240を開放する必要は無い(むしろ開けてはいけない)。
#
# attach/detach/list/statusはすべてこのコンテナ内で実行する。ただしattachの実体である
# vhci_hcdはホストLinuxのカーネルモジュールなので、ホスト側で一度だけ
# `sudo modprobe vhci-hcd`が要る。attachで生えた/dev/ttyUSB0はホストの/devに現れ、
# /devバインドマウント(docker-compose.yml)でコンテナからもそのまま見える。
#
set -euo pipefail

USBIP_HOST="${USBIP_HOST:-127.0.0.1}"
USBIP_BUSID="${USBIP_BUSID:-}"

usage() {
    cat <<'EOF'
使い方: usb-attach.sh <サブコマンド> [引数...]
        (すべてdevcontainer内のターミナルで実行する)

サブコマンド:
  attach [host] [busid]  USB/IP経由で基板をアタッチする(/dev/ttyUSB0が生える)
  detach                 アタッチを解除する
  list [host]            リモート(Windows側)で共有されているデバイス一覧を表示する
  status                 現在のアタッチ状況を表示する

環境変数:
  USBIP_HOST       SSHトンネルの出口アドレス(既定値127.0.0.1。通常は変更不要)
  USBIP_BUSID      共有されているデバイスのbusid(引数でも上書き可能)
  USBIP_BIN        usbipコマンドの実体パスを明示指定したい場合に使う
  USBIP_ALLOW_HOST 1にするとコンテナ外での実行チェックを外す(後述の代替経路用)

前提:
  1. ホストLinuxで一度だけ`sudo modprobe vhci-hcd`(attachの実体はホストのカーネル)。
  2. Windows側でusbipd-winのインストールと
       usbipd bind --busid <BUSID>
     を実行しておく(bindはWindows再起動まで維持される)。
  3. VS Codeでdevcontainerを開く(2222が自動でWindowsのlocalhostへ転送される)。
  4. Windows側の~/.ssh/configに`Host devcontainer`エントリ(Port 2222 /
     RemoteForward 3240 127.0.0.1:3240)を作り、PowerShellで
       ssh -N devcontainer
     を実行したまま置いておく。
  詳細はREADMEの「実機接続(USB/IP)」節を参照。

例:
  .devcontainer/usb-attach.sh attach 127.0.0.1 2-3
  USBIP_BUSID=2-3 .devcontainer/usb-attach.sh attach
  .devcontainer/usb-attach.sh list 127.0.0.1
  .devcontainer/usb-attach.sh status
  .devcontainer/usb-attach.sh detach

代替経路:
  コンテナ内からのattachが動かない場合に限り、Windows側のRemoteForward先を
  ホストLinuxのsshdへ変え、ホストLinuxで
    USBIP_ALLOW_HOST=1 .devcontainer/usb-attach.sh attach 127.0.0.1 <BUSID>
  を実行する。生えた/dev/ttyUSB0は/devバインドマウントでコンテナからも見える。
EOF
}

# コンテナ内で実行されているかどうかを判定する。
# devcontainerはDocker上で動くため/.dockerenvが存在する。
in_container() {
    [ -e /.dockerenv ]
}

# attach/detachはこのコンテナ内で実行する(Windowsからの逆トンネルの出口が
# コンテナ自身の127.0.0.1:3240にあるため)。ホストLinuxから呼ばれても、その口は
# ホストには開いていないので届かない。誤実行を実行前に止める。
# USBIP_ALLOW_HOST=1のときだけこの確認を外す(usageの「代替経路」を参照)。
require_container() {
    local example="$1"
    if [ "${USBIP_ALLOW_HOST:-0}" = "1" ]; then
        return 0
    fi
    if ! in_container; then
        cat >&2 <<EOF
エラー: この操作はdevcontainer内のターミナルで実行すること。
Windowsからの逆トンネルはコンテナの127.0.0.1:3240に口を開くため、
ホストLinuxからは3240番へ到達できない。
VS Codeでdevcontainerを開き、その中のターミナルで以下を実行すること:
  ${example}
(ホスト側で実行する代替経路を使う場合はUSBIP_ALLOW_HOST=1を付ける)
EOF
        exit 1
    fi
}

# usbipコマンドの実体を探して標準出力へパスを返す。
# Ubuntuの/usr/bin/usbipはuname -rでバイナリを探すシェルラッパーだが、
# コンテナが見るカーネルバージョンとlinux-toolsパッケージのバージョンが
# ずれていると動かない(ホスト側でも同じ問題が起き得る)。
resolve_usbip() {
    if [ -n "${USBIP_BIN:-}" ]; then
        echo "${USBIP_BIN}"
        return 0
    fi

    if command -v usbip > /dev/null 2>&1 && usbip version > /dev/null 2>&1; then
        command -v usbip
        return 0
    fi

    local candidate
    candidate="$(ls /usr/lib/linux-tools/*/usbip 2>/dev/null | head -n1 || true)"
    if [ -n "$candidate" ]; then
        echo "$candidate"
        return 0
    fi

    echo "エラー: usbipコマンドの実体が見つからない。" >&2
    echo "sudo apt install linux-tools-genericを実行してから再試行すること。" >&2
    echo "(Ubuntu標準の/usr/bin/usbipはカーネルバージョンでバイナリを探すラッパーのため、" >&2
    echo "カーネルとlinux-toolsのバージョンが一致していないと機能しない)" >&2
    return 1
}

# SSHのリバースフォワード経由で127.0.0.1:3240へ到達できるか確認する。
# usbip attachもusbip listも内部でこのポートへ接続するため、事前に切り分ける。
check_tunnel() {
    local host="$1"
    if timeout 3 bash -c "exec 3<>/dev/tcp/${host}/3240" 2>/dev/null; then
        return 0
    fi
    return 1
}

show_status() {
    echo "--- usbip port ---"
    sudo "${USBIP}" port || true
    echo "--- /dev/ttyUSB* ---"
    ls -l /dev/ttyUSB* 2>/dev/null || echo "(見つからない)"
}

find_attached_port() {
    # "Port 00: <Port in Use> ..."のような行から番号を取り出す。
    # アタッチ済みポートが無ければ空文字を返す(grepが該当無しで非0終了しても
    # set -eでスクリプトごと落ちないよう、パイプライン全体の終了ステータスを潰す)。
    sudo "${USBIP}" port 2>/dev/null \
        | grep -E '^Port [0-9]+: <Port in Use>' \
        | head -n1 \
        | sed -E 's/^Port ([0-9]+):.*/\1/' \
        || true
}

cmd_attach() {
    require_container ".devcontainer/usb-attach.sh attach ${USBIP_HOST} <BUSID>"

    local host="${1:-${USBIP_HOST}}"
    local busid="${2:-${USBIP_BUSID}}"

    USBIP="$(resolve_usbip)" || exit 1

    if compgen -G "/dev/ttyUSB*" > /dev/null 2>&1; then
        echo "警告: /dev/ttyUSB*が既に存在する。アタッチ済みに見えるので二重アタッチはしない。"
        show_status
        exit 0
    fi

    if ! check_tunnel "$host"; then
        cat >&2 <<EOF
エラー: ${host}:3240へ接続できない。逆トンネルが張られていない可能性がある。
以下を確認すること:
  - Windows側のPowerShellで`ssh -N devcontainer`を実行したまま置いてあるか
    (無言で止まって見えるのが正常。閉じるとトンネルも切れる)
  - Windows側の~/.ssh/configのdevcontainerエントリにPort 2222と
    RemoteForward 3240 127.0.0.1:3240を書いたか
  - Windows側でusbipd bind --busid <BUSID>を実行済みか
  - VS Codeでdevcontainerを開き直したか(2222の転送は接続中しか存在しない)
EOF
        exit 1
    fi

    if [ -z "$busid" ]; then
        echo "エラー: busidが指定されていない。以下はWindows側(${host})で共有されているデバイス一覧:" >&2
        sudo "${USBIP}" list -r "$host" || echo "  (一覧取得に失敗した。トンネルの状態を確認すること)" >&2
        echo "USBIP_BUSID環境変数か、attachの第2引数でbusidを指定すること。" >&2
        exit 1
    fi

    echo "${host}のbusid ${busid}をアタッチする..."
    if ! sudo "${USBIP}" attach -r "$host" -b "$busid"; then
        echo "エラー: usbip attachに失敗した。以下を確認すること:" >&2
        echo "  - Windows側でusbipd bind --busid ${busid}を実行済みか" >&2
        echo "  - SSHのリバースフォワードが生きているか(VSCodeを再接続してみる)" >&2
        exit 1
    fi

    echo -n "/dev/ttyUSB*が現れるのを待っている"
    local waited=0
    while ! compgen -G "/dev/ttyUSB*" > /dev/null 2>&1; do
        if [ "$waited" -ge 10000 ]; then
            echo
            echo "エラー: 10秒待ったが/dev/ttyUSB*が現れなかった。" >&2
            echo "usbip attach自体は成功しているので、USBシリアルのドライバやudevルールを確認すること。" >&2
            exit 1
        fi
        echo -n "."
        sleep 0.5
        waited=$((waited + 500))
    done
    echo
    ls -l /dev/ttyUSB*
    echo "アタッチ成功。devcontainer内でidf.py -p /dev/ttyUSB0 flash monitorが実行できる。"
}

cmd_detach() {
    require_container ".devcontainer/usb-attach.sh detach"

    USBIP="$(resolve_usbip)" || exit 1

    local port
    port="$(find_attached_port)"
    if [ -z "$port" ]; then
        echo "アタッチされていない。何もしない。"
        exit 0
    fi
    echo "ポート${port}をデタッチする..."
    sudo "${USBIP}" detach -p "$port"
    echo "デタッチ完了。"
}

cmd_list() {
    local host="${1:-${USBIP_HOST}}"

    USBIP="$(resolve_usbip)" || exit 1

    sudo "${USBIP}" list -r "$host"
}

cmd_status() {
    USBIP="$(resolve_usbip)" || exit 1
    show_status
}

case "${1:-}" in
    attach)
        shift
        cmd_attach "$@"
        ;;
    detach)
        cmd_detach
        ;;
    list)
        shift
        cmd_list "$@"
        ;;
    status)
        cmd_status
        ;;
    *)
        usage
        exit 1
        ;;
esac
