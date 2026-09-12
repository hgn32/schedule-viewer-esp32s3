#!/usr/bin/env python3
"""デバイスの画面をログから復元するホスト側スクリプト(一時的な開発用)。

こちらからデバイスの画面を直接確認する手段が無いため、`Display::dumpScreenshot()`
(`main/display.cpp`)がスプライトの内容を縦横それぞれ1/2に間引いた300x512の
RGB565をBase64で1行ずつ`ESP_LOG*`(`SHOT`で始まる行)へ出している。本スクリプトは
その出力を`logs/monitor.log`から拾い集めてPNGへ復元する。

サーバ到達性が確認でき、ダミーモード自体(USE_DUMMY_SCHEDULE)が撤去できる時点で、
この機能(`Display::dumpScreenshot()`とこのスクリプト)ごと削除してよい。

使い方:
    python3 tools/screenshot.py [入力ログ] [出力PNG]

既定値は入力logs/monitor.log、出力logs/screenshot.png。

ログの形式:
    SHOT BEGIN <幅> <高さ>
    SHOT <行番号> <Base64(RGB565、リトルエンディアン)>
    ...
    SHOT END

同じログに複数回分のSHOTブロックが含まれている場合は、最後のブロックを使う。
行が欠けている場合はその行を黒で埋め、欠けた行数を標準エラーへ警告として出す
(シリアルの取りこぼしを黙って隠さないため)。

PNGの書き出しはPillowを使わず、標準ライブラリのzlibとstructだけで行う
(非圧縮ではなくzlibで圧縮する)。
"""

import base64
import re
import struct
import sys
import zlib

DEFAULT_INPUT = "logs/monitor.log"
DEFAULT_OUTPUT = "logs/screenshot.png"

RE_BEGIN = re.compile(r"SHOT BEGIN (\d+) (\d+)")
RE_LINE = re.compile(r"SHOT (\d+) (\S+)")
RE_END = re.compile(r"SHOT END")


def parseLog(path):
    """ログからSHOTブロックを読み取る。複数ブロックがあれば最後のものを返す。

    戻り値は(width, height, {行番号: Base64文字列})のタプル。
    見つからなければNoneを返す。
    """
    blocks = []
    cur_w = None
    cur_h = None
    cur_lines = None

    with open(path, "r", encoding="utf-8", errors="replace") as f:
        for raw_line in f:
            m = RE_BEGIN.search(raw_line)
            if m:
                cur_w = int(m.group(1))
                cur_h = int(m.group(2))
                cur_lines = {}
                continue

            if cur_lines is not None:
                m = RE_LINE.search(raw_line)
                if m:
                    idx = int(m.group(1))
                    b64 = m.group(2)
                    cur_lines[idx] = b64
                    continue

                if RE_END.search(raw_line):
                    blocks.append((cur_w, cur_h, cur_lines))
                    cur_w = None
                    cur_h = None
                    cur_lines = None
                    continue

    if not blocks:
        return None
    return blocks[-1]


def rgb565ToRgb888(lo, hi):
    """RGB565(リトルエンディアン、下位バイトlo・上位バイトhi)をRGB888(r, g, b)へ。

    main/display.cppはuint16_tをそのままバイト列にして書き出しており、ESP32は
    リトルエンディアンなので、先に来るバイトが下位バイトになる。ビット並びは
    LovyanGFXのrgb565_nonswapped(RRRRRGGGGGGBBBBB)と同じ。
    """
    value = lo | (hi << 8)
    r5 = (value >> 11) & 0x1F
    g6 = (value >> 5) & 0x3F
    b5 = value & 0x1F
    r = (r5 * 255 + 15) // 31
    g = (g6 * 255 + 31) // 63
    b = (b5 * 255 + 15) // 31
    return r, g, b


def buildImage(width, height, lines):
    """行番号→Base64の辞書から、RGB888の生ピクセル配列(行ごとフィルタバイト付き)を作る。

    欠けている行は黒で埋め、欠けた行数を標準エラーへ警告として出す。
    """
    missing = 0
    raw = bytearray()

    for y in range(height):
        raw.append(0)  # PNGのフィルタタイプ(0: None)

        b64 = lines.get(y)
        if b64 is None:
            missing += 1
            raw.extend(bytes(width * 3))
            continue

        try:
            row_bytes = base64.b64decode(b64)
        except Exception as e:
            print(f"警告: 行{y}のBase64デコードに失敗した({e})、黒で埋める",
                  file=sys.stderr)
            missing += 1
            raw.extend(bytes(width * 3))
            continue

        if len(row_bytes) < width * 2:
            print(f"警告: 行{y}のデータが不足している"
                  f"({len(row_bytes)}バイト、期待値{width * 2}バイト)、黒で埋める",
                  file=sys.stderr)
            missing += 1
            raw.extend(bytes(width * 3))
            continue

        for x in range(width):
            lo = row_bytes[x * 2]
            hi = row_bytes[x * 2 + 1]
            r, g, b = rgb565ToRgb888(lo, hi)
            raw.append(r)
            raw.append(g)
            raw.append(b)

    if missing > 0:
        print(f"警告: {missing}行が欠損していたため黒で埋めた(全{height}行中)",
              file=sys.stderr)

    return bytes(raw)


def pngChunk(chunk_type, data):
    """PNGの1チャンク(長さ + タイプ + データ + CRC)を作る。"""
    type_and_data = chunk_type + data
    crc = zlib.crc32(type_and_data) & 0xFFFFFFFF
    return struct.pack(">I", len(data)) + type_and_data + struct.pack(">I", crc)


def writePng(path, width, height, raw_with_filter):
    """非圧縮ではなくzlibで圧縮したPNG(カラータイプ2、RGB888、8bit)を書き出す。"""
    signature = b"\x89PNG\r\n\x1a\n"

    ihdr = struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)

    compressed = zlib.compress(raw_with_filter, level=9)

    with open(path, "wb") as f:
        f.write(signature)
        f.write(pngChunk(b"IHDR", ihdr))
        f.write(pngChunk(b"IDAT", compressed))
        f.write(pngChunk(b"IEND", b""))


def main():
    input_path = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_INPUT
    output_path = sys.argv[2] if len(sys.argv) > 2 else DEFAULT_OUTPUT

    block = parseLog(input_path)
    if block is None:
        print(f"エラー: {input_path}にSHOTブロックが見つからない", file=sys.stderr)
        sys.exit(1)

    width, height, lines = block
    raw = buildImage(width, height, lines)
    writePng(output_path, width, height, raw)
    print(f"{output_path}を書き出した({width}x{height}、{len(lines)}/{height}行受信)")


if __name__ == "__main__":
    main()
