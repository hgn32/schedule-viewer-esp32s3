#!/usr/bin/env bash
# Windows側がscpで取りに来る転送物をbuild/winflash/へまとめる(devcontainer内で実行)。
#
#   build/winflash/
#     lib/            esptool / serial(pyserial) / intelhex   ← 純Python。Windowsで
#                     PYTHONPATHを通すだけで動く(インストール不要)
#     build/          flash_args と .bin 一式
#     win_flash.py    Windows側で走る書き込み・監視スクリプト
#
# 転送物の実体はすべてこのコンテナ側にあるので、Windows側の作業フォルダは
# いつ消えても再取得できる。
set -euo pipefail

PROJ="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="${PROJ}/build"
OUT="${BUILD}/winflash"

if [ ! -f "${BUILD}/flash_args" ]; then
    echo "エラー: ${BUILD}/flash_argsが無い。先にidf.py buildを実行すること。" >&2
    exit 1
fi

# IDFのvenvのsite-packages。バージョン名が変わっても拾えるようglobで探す。
SITE_PACKAGES="$(ls -d /opt/esp/python_env/*/lib/python*/site-packages 2>/dev/null | head -n1 || true)"
if [ -z "${SITE_PACKAGES}" ]; then
    echo "エラー: ESP-IDFのsite-packagesが見つからない。" >&2
    exit 1
fi

rm -rf "${OUT}"
mkdir -p "${OUT}/lib" "${OUT}/build"

for pkg in esptool serial intelhex; do
    if [ ! -d "${SITE_PACKAGES}/${pkg}" ]; then
        echo "エラー: ${SITE_PACKAGES}/${pkg}が無い。" >&2
        exit 1
    fi
    cp -r "${SITE_PACKAGES}/${pkg}" "${OUT}/lib/"
done
# Linuxで生成された.pycは不要(Windows側でも生成させない)
find "${OUT}/lib" -name __pycache__ -type d -prune -exec rm -rf {} + 2>/dev/null || true

# flash_argsの2行目以降が「オフセット 相対パス」。ここを読んで必要な.binだけ複製する。
cp "${BUILD}/flash_args" "${OUT}/build/"
while read -r _offset relpath; do
    [ -n "${relpath:-}" ] || continue
    mkdir -p "${OUT}/build/$(dirname "${relpath}")"
    cp "${BUILD}/${relpath}" "${OUT}/build/${relpath}"
done < <(tail -n +2 "${BUILD}/flash_args")

cp "${PROJ}/tools/win_flash.py" "${OUT}/"

mkdir -p "${PROJ}/logs"

echo "作成した: ${OUT}"
du -sh "${OUT}"
