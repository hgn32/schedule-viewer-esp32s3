#!/usr/bin/env python3
"""Windows側で実行する書き込み・シリアル監視スクリプト。

devcontainerからscpで取得したesptool一式(lib/)とビルド成果物(build/)を使い、
ESP32-S3-Touch-LCD-7BのUSB-TO-UARTポートへネイティブに書き込む。USB/IPは使わない。

USB/IP経由の書き込みが遅い理由(M5Paper時代に実測。同じUSB/IP構成を使う限り
今回の基板でも同様の制約になる見込みだが、ブリッジチップが変わるため数値は未検証):
  cp210xドライバは書き込みURBが256バイト、同時発行が2本(kernelの
  usb_serial_portのwrite_urbs[2])。USB/IPはURB1個をTCPの要求/応答1組に
  載せるため、512バイト/往復が上限になる。実測RTT110msでは約4.4KB/sで、
  圧縮後1.76MBの書き込みに6分以上かかる(実測367秒 / 51.5kbit/s)。
  ボーレートを上げても待ち時間は減らないので、書き込みはWindows側で行う。

出力はすべて1行ずつssh経由でdevcontainerの/workspaces/logs/へ流し込む。
「Windows側にしか無い情報」を作らないことが目的なので、終了時の一括
アップロードはしない(sshが切れている間だけローカルの控えに退避し、
再接続時に送り直す)。

使い方(通常は.devcontainer/flash.ps1から呼ばれる):
  python win_flash.py flash   --port COM3 --baud 921600 --ssh devcontainer --workdir <dir>
  python win_flash.py monitor --port COM3 --baud 115200 --ssh devcontainer --workdir <dir>

依存はpyserialのみ。PYTHONPATHでlib/を指しておくこと(インストールは不要)。
"""

import argparse
import datetime
import os
import subprocess
import sys
import time

REMOTE_LOG_DIR = "/workspaces/logs"
RECONNECT_INTERVAL = 3.0     # sshが切れてから再接続を試みるまでの秒数
ACK_AFTER = 5.0              # 接続がこの秒数生きていれば送信済みとみなす
RESEND_LIMIT = 64 * 1024     # 再接続時に送り直す最大バイト数


def isoNow():
    return datetime.datetime.now().astimezone().isoformat(timespec="seconds")


class LogStream:
    """1行ずつ「ローカルの控え」と「ssh経由のdevcontainer」の両方へ書く。

    sshは接続先が死んでいてもPopen自体は成功してしまうため、「書けた=届いた」
    とは判定できない。そこで接続がACK_AFTER秒生き延びた時点までを送信済みと
    みなし、切断を検知したらそれ以降を送り直す。ログ用途なので、
    重複は許容し欠落は許容しない方針にしている。
    """

    def __init__(self, sshHost, remoteName, localPath, echo=True):
        self.sshHost = sshHost
        self.remotePath = "%s/%s" % (REMOTE_LOG_DIR, remoteName)
        self.localPath = localPath
        self.echo = echo
        self.proc = None
        self.connStart = 0.0
        self.nextRetryAt = 0.0
        self.ackOffset = 0        # ここまでは届いたとみなすローカル控えのバイト位置
        os.makedirs(os.path.dirname(localPath), exist_ok=True)
        self.localFile = open(localPath, "ab")
        self.ackOffset = self.localFile.tell()
        self._connect()

    # --- 接続 ---------------------------------------------------------------

    def _connect(self):
        if self.proc is not None and self.proc.poll() is None:
            return True
        if self.proc is not None:
            self.proc = None
        if time.time() < self.nextRetryAt:
            return False
        cmd = [
            "ssh", "-o", "BatchMode=yes", "-o", "ServerAliveInterval=30",
            self.sshHost,
            "mkdir -p %s && cat >> %s" % (REMOTE_LOG_DIR, self.remotePath),
        ]
        try:
            self.proc = subprocess.Popen(
                cmd, stdin=subprocess.PIPE, stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
            )
        except OSError:
            self.proc = None
            self.nextRetryAt = time.time() + RECONNECT_INTERVAL
            return False
        self.connStart = time.time()
        self.nextRetryAt = 0.0
        self._resendFromAck()
        return True

    def _resendFromAck(self):
        """未送信とみなす範囲をローカルの控えから読み直して送る。"""
        self.localFile.flush()
        end = self.localFile.tell()
        start = self.ackOffset
        if end <= start:
            return
        if end - start > RESEND_LIMIT:
            start = end - RESEND_LIMIT
        with open(self.localPath, "rb") as f:
            f.seek(start)
            data = f.read(end - start)
        header = ("=== RESYNC %s %dバイトを送り直す(重複する場合がある) ===\n"
                  % (isoNow(), len(data))).encode("utf-8")
        try:
            self.proc.stdin.write(header)
            self.proc.stdin.write(data)
            self.proc.stdin.flush()
        except (BrokenPipeError, OSError, ValueError):
            self._dropConnection()

    def _dropConnection(self):
        self.proc = None
        self.nextRetryAt = time.time() + RECONNECT_INTERVAL

    # --- 書き込み -----------------------------------------------------------

    def write(self, line):
        line = line.rstrip("\r\n")
        if self.echo:
            # Windowsのコンソールの既定はCP932で、esptoolの出力に含まれる文字を
            # 表示できずにUnicodeEncodeErrorで落ちることがある。表示は補助的な
            # 機能なので、失敗しても本体(ローカル控えとコンテナへの送信)は続ける。
            try:
                sys.stdout.write(line + "\n")
                sys.stdout.flush()
            except (UnicodeEncodeError, OSError, ValueError):
                pass
        raw = (line + "\n").encode("utf-8", "replace")
        self.localFile.write(raw)
        self.localFile.flush()

        if not self._connect():
            return
        try:
            self.proc.stdin.write(raw)
            self.proc.stdin.flush()
        except (BrokenPipeError, OSError, ValueError):
            self._dropConnection()
            return
        # 接続が十分に生き延びたら、そこまでを届いたとみなす
        if time.time() - self.connStart > ACK_AFTER:
            self.ackOffset = self.localFile.tell()

    def close(self):
        if self.proc is not None and self.proc.poll() is None:
            try:
                self.proc.stdin.close()
            except OSError:
                pass
            try:
                self.proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                self.proc.kill()
        self.localFile.close()


# --- サブコマンド -----------------------------------------------------------

def cmdFlash(args):
    """esptoolを子プロセスで起動し、出力を1行ずつ流す。"""
    stream = LogStream(args.ssh, "flash.log",
                       os.path.join(args.workdir, "logs", "flash.log"))
    buildDir = os.path.join(args.workdir, "build")
    rc = 1
    try:
        stream.write("=== FLASH START %s port=%s baud=%d ==="
                     % (isoNow(), args.port, args.baud))
        cmd = [
            sys.executable, "-m", "esptool",
            "--chip", "esp32s3", "--port", args.port, "--baud", str(args.baud),
            "--before", "default_reset", "--after", "hard_reset",
            "write_flash", "@flash_args",
        ]
        stream.write("$ " + " ".join(cmd))
        # esptoolはworkdir/lib配下にコピーしただけでインストールはしていない。
        # 子プロセスはbuild/へcdして動くので、PYTHONPATHは絶対パスで渡す
        # (相対パスのまま渡すとcd先を基準に解決されて見つからない)。
        env = os.environ.copy()
        libDir = os.path.abspath(os.path.join(args.workdir, "lib"))
        env["PYTHONPATH"] = os.pathsep.join(
            [libDir] + ([env["PYTHONPATH"]] if env.get("PYTHONPATH") else []))
        env["PYTHONDONTWRITEBYTECODE"] = "1"
        proc = subprocess.Popen(
            cmd, cwd=buildDir, env=env, stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT, bufsize=1, text=True,
            encoding="utf-8", errors="replace",
        )
        for line in proc.stdout:
            stream.write(line)
        rc = proc.wait()
    except KeyboardInterrupt:
        rc = 130
    except Exception as e:  # 想定外でも必ず結果行を残す(無言=成功と誤読させない)
        stream.write("EXCEPTION: %r" % (e,))
    finally:
        stream.write("=== FLASH RESULT rc=%d %s ===" % (rc, isoNow()))
        stream.close()
    return rc


def cmdMonitor(args):
    """COMポートを読み、1行ずつ流す。Ctrl+Cで終了する。"""
    import serial  # lib/(pyserial)から読む

    stream = LogStream(args.ssh, "monitor.log",
                       os.path.join(args.workdir, "logs", "monitor.log"))
    rc = 0
    ser = None
    try:
        stream.write("=== MONITOR START %s port=%s baud=%d ==="
                     % (isoNow(), args.port, args.baud))
        ser = serial.Serial(args.port, args.baud, timeout=0.1)
        buf = bytearray()
        lastData = time.time()
        deadline = time.time() + args.seconds if args.seconds > 0 else None
        while deadline is None or time.time() < deadline:
            chunk = ser.read(max(1, ser.in_waiting))
            if chunk:
                buf.extend(chunk)
                while b"\n" in buf:
                    line, _, rest = buf.partition(b"\n")
                    buf = bytearray(rest)
                    stream.write(line.decode("utf-8", "replace"))
                lastData = time.time()
            elif buf and time.time() - lastData > 0.3:
                # 改行が来ない断片も遅延なく出す
                stream.write(buf.decode("utf-8", "replace"))
                buf = bytearray()
                lastData = time.time()
    except KeyboardInterrupt:
        rc = 0
    except Exception as e:
        stream.write("EXCEPTION: %r" % (e,))
        rc = 1
    finally:
        if ser is not None:
            try:
                ser.close()
            except Exception:
                pass
        stream.write("=== MONITOR END rc=%d %s ===" % (rc, isoNow()))
        stream.close()
    return rc


def main():
    # 標準出力・標準エラーをUTF-8にする。Windowsの既定(CP932)のままだと、
    # esptoolの出力に含まれる文字を表示できずに落ちる。
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(encoding="utf-8", errors="replace")
        except Exception:
            pass

    parser = argparse.ArgumentParser(
        description="ESP32-S3-Touch-LCD-7BへWindows側から書き込む / シリアルログを取る")
    sub = parser.add_subparsers(dest="mode", required=True)
    for name, defaultBaud in (("flash", 921600), ("monitor", 115200)):
        p = sub.add_parser(name)
        p.add_argument("--port", required=True, help="例: COM3")
        p.add_argument("--baud", type=int, default=defaultBaud)
        p.add_argument("--ssh", default="devcontainer",
                       help="~/.ssh/configのホスト名")
        p.add_argument("--workdir", required=True,
                       help="lib/ build/ logs/ を置く作業フォルダ")
        if name == "monitor":
            p.add_argument("--seconds", type=int, default=0,
                           help="この秒数で監視を打ち切る(0は無制限)")
    args = parser.parse_args()
    return cmdFlash(args) if args.mode == "flash" else cmdMonitor(args)


if __name__ == "__main__":
    sys.exit(main())
