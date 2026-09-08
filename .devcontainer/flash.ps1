# Windows側で実行する書き込み一括スクリプト(.devcontainer/flash.ps1)。
#
# ビルド(コンテナ) → 成果物の取得 → COMポートへ書き込み → シリアル監視 まで通しで行う。
# USB/IP経由の書き込みは1URBあたり256バイト×2本=512バイト/往復に制限され、
# 実測で6分以上かかるため、書き込みだけWindows側のネイティブUSBで行う。
#
# 通信はすべてWindows発(既存の`Host devcontainer`のSSH)。Windows側には
# 常駐プロセスもインストールも作らず、作業フォルダ1つだけを使う。
#
# 実行例(ファイルをWindowsに置かずに走らせる):
#   & ([scriptblock]::Create((ssh devcontainer "cat /workspaces/.devcontainer/flash.ps1") -join "`n")) -Port COM3

param(
    [string]$Port = "COM3",
    [int]$Baud = 921600,
    [int]$MonitorBaud = 115200,
    [string]$SshHost = "devcontainer",
    [string]$WorkDir = "$env:TEMP\m5flash",
    [switch]$SkipBuild,
    [switch]$NoMonitor,
    [switch]$Clean
)

$ErrorActionPreference = "Stop"

if ($Clean) {
    Remove-Item -Recurse -Force $WorkDir -ErrorAction SilentlyContinue
    Write-Host "削除した: $WorkDir"
    return
}

New-Item -ItemType Directory -Force "$WorkDir", "$WorkDir\logs" | Out-Null
Start-Transcript -Path "$WorkDir\logs\win.log" -Force | Out-Null

try {
    # --- 1. ビルド(コンテナ内。ログもコンテナ側に残る) ---------------------
    if (-not $SkipBuild) {
        Write-Host "[1/4] ビルド(devcontainer)"
        ssh $SshHost @"
set -e
cd /workspaces
mkdir -p logs
. /opt/esp/idf/export.sh >/dev/null 2>&1
idf.py build 2>&1 | tee logs/build.log
tools/stage-winflash.sh
"@
        if ($LASTEXITCODE -ne 0) { throw "ビルドまたはステージングに失敗した(exit $LASTEXITCODE)。logs/build.logを参照" }
    }

    # --- 2. USB/IPのアタッチを外す(WindowsがCOMポートを掴めるようにする) ---
    Write-Host "[2/4] USB/IPのデタッチ"
    ssh $SshHost "/workspaces/.devcontainer/usb-attach.sh detach" | Out-Host

    # --- 3. 転送物の取得 ----------------------------------------------------
    Write-Host "[3/4] 成果物の取得"
    Remove-Item -Recurse -Force "$WorkDir\build" -ErrorAction SilentlyContinue
    scp -r "${SshHost}:/workspaces/build/winflash/build" "$WorkDir"
    if ($LASTEXITCODE -ne 0) { throw "scpに失敗した(exit $LASTEXITCODE)" }
    scp "${SshHost}:/workspaces/build/winflash/win_flash.py" "$WorkDir"
    if (-not (Test-Path "$WorkDir\lib\esptool")) {
        # esptool一式は初回だけ。消えていれば取り直す。
        scp -r "${SshHost}:/workspaces/build/winflash/lib" "$WorkDir"
    }

    # --- 4. 書き込みと監視 --------------------------------------------------
    # PYTHONPATHはこのウィンドウ限り。site-packagesにもレジストリにも触れない。
    $env:PYTHONPATH = "$WorkDir\lib"
    $env:PYTHONDONTWRITEBYTECODE = "1"

    Write-Host "[4/4] 書き込み"
    python "$WorkDir\win_flash.py" flash --port $Port --baud $Baud --ssh $SshHost --workdir $WorkDir
    $flashRc = $LASTEXITCODE
    if ($flashRc -ne 0) { throw "書き込みに失敗した(exit $flashRc)。logs/flash.logを参照" }

    if (-not $NoMonitor) {
        Write-Host "監視を開始する(Ctrl+Cで終了)"
        python "$WorkDir\win_flash.py" monitor --port $Port --baud $MonitorBaud --ssh $SshHost --workdir $WorkDir
    }
}
finally {
    Stop-Transcript | Out-Null
    # transcriptは仕組み上ファイルにしか書けないので、ここだけ終了時にコピーする。
    # 重要な情報(各ステップの結果)はwin_flash.pyがライブでlogs/flash.logへ流している。
    scp "$WorkDir\logs\win.log" "${SshHost}:/workspaces/logs/win.log" 2>$null
}
