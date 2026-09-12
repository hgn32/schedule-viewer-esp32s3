# devcontainerからの指示でESP32-S3-Touch-LCD-7Bへの書き込み・監視を行う、Windows側の実行スクリプト。
#
# なぜ必要か:
#   実機への書き込みはWindows側のネイティブUSBで行う(USB/IP経由では1往復512バイトに
#   制限され6分以上かかる)。devcontainerからWindowsへ発信することはできないため、
#   Windows側から張ったsshの標準出力を指示の受け口として使う。
#
#   コンテナ: tools/win.sh --> FIFO --> tools/win-agent.sh
#                                        |
#                                        | (sshの標準出力を逆流)
#                                        v
#   Windows : このスクリプトが「決められた6つの操作」だけを実行する
#                                        |
#                                        v
#   コンテナ: logs/win/(ID).log (末尾に "=== EXIT rc=N ===")
#
# 受け付けるのは下記の6つだけで、任意のコマンドは実行しない。
# コンテナ側から渡せるのはポート番号などのパラメータのみで、書式も検証する。
#
#   sync                          転送物(esptool一式と.bin)をscpで取得する
#   ports                         COMポートの一覧を出す
#   probe   port=COM3             chip_id(書き換えは起きない)
#   reset   port=COM3             アプリを起動させる(esptool run。書き換えは起きない)
#   flash   port=COM3 baud=N      書き込み
#   monitor port=COM3 sec=N       シリアルログの取得
#
# Windows側で待ち受けるポートは作らない。接続は常にWindows発。
# インストール・サービス登録・レジストリ変更もしない。使うのはssh / scp / pythonだけ。
#
# 注意: このファイルはUTF-8(BOM付き)で保存すること。
#       Windows PowerShell 5.1はBOMが無いUTF-8をCP932として読むため、日本語が壊れて
#       構文エラーになる。自己更新時もBOM付きで書き戻している。
#
# 初回だけ取得する:
#   scp devcontainer:/workspaces/.devcontainer/win-agent.ps1 .
#
# 開発のたびに起動する(既定の実行ポリシーでは.ps1を実行できないためBypassを付ける。
# このプロセス限りの指定で、PCの設定は変更しない):
#   powershell -NoProfile -ExecutionPolicy Bypass -File .\win-agent.ps1

param(
    [string]$SshHost = "devcontainer",
    [string]$WorkDir = "$env:LOCALAPPDATA\m5flash",
    [string]$Python = "",
    [string]$Ssh = "",
    [string]$Scp = "",
    [switch]$NoSelfUpdate
)

$ErrorActionPreference = "Continue"
New-Item -ItemType Directory -Force $WorkDir, "$WorkDir\logs" | Out-Null

# --- 外部コマンドの実体を解決する --------------------------------------------
# -NoProfileで起動するとPATHがプロファイルで拡張されないため、ssh/scpが
# 見つからないことがある。既定の設置場所も候補に入れて探す。
function Get-ExeCandidates([string]$name) {
    $list = New-Object System.Collections.ArrayList
    try {
        $found = & where.exe $name 2>$null
        if ($found) {
            foreach ($p in $found) {
                if ($p) { [void]$list.Add($p.Trim()) }
            }
        }
    } catch { }
    [void]$list.Add("C:\Windows\System32\OpenSSH\$name.exe")
    [void]$list.Add("C:\Windows\Sysnative\OpenSSH\$name.exe")
    if ($env:WINDIR) {
        [void]$list.Add((Join-Path $env:WINDIR "System32\OpenSSH\$name.exe"))
        [void]$list.Add((Join-Path $env:WINDIR "Sysnative\OpenSSH\$name.exe"))
    }
    if ($env:ProgramFiles) {
        [void]$list.Add((Join-Path $env:ProgramFiles "OpenSSH\$name.exe"))
        [void]$list.Add((Join-Path $env:ProgramFiles "Git\usr\bin\$name.exe"))
    }
    return $list
}

function Resolve-Exe([string]$name) {
    foreach ($c in (Get-ExeCandidates $name)) {
        if ($c -and ($c -notmatch "WindowsApps") -and (Test-Path -LiteralPath $c)) { return $c }
    }
    return ""
}

# 見つからなかったときに、何を試したのかを必ず表示する(推測で切り分けさせないため)
function Show-ExeSearch([string]$name) {
    Write-Host "--- $name の探索結果 ---"
    foreach ($c in (Get-ExeCandidates $name)) {
        $ok = $false
        try { $ok = Test-Path -LiteralPath $c } catch { }
        Write-Host ("  {0}  {1}" -f $(if ($ok) { "あり" } else { "なし" }), $c)
    }
    Write-Host ("  WINDIR={0}" -f $env:WINDIR)
    Write-Host ("  64bitプロセス={0}" -f [Environment]::Is64BitProcess)
    Write-Host ("  PATH={0}" -f $env:PATH)
}

# Pythonの実体。PATHの先頭にMicrosoft Storeのスタブ(WindowsApps\python.exe)が
# 入っていることがあり、素のpythonを呼ぶとそれを掴んで失敗する。
function Resolve-Python {
    if ($Python) { return $Python }
    try {
        $exe = & py -3 -c "import sys; print(sys.executable)" 2>$null
        if ($LASTEXITCODE -eq 0 -and $exe) { return $exe.Trim() }
    } catch { }
    return (Resolve-Exe "python")
}

# --- 自己更新 ----------------------------------------------------------------
# コンテナ側のwin-agent.ps1と中身が違えば入れ替えて起動し直す。
# 修正のたびに人手でscpし直さなくて済むようにするため。
function Normalize-Script([string]$text) {
    if ($null -eq $text) { return "" }
    return $text.Replace([string][char]0xFEFF, "").Replace("`r", "").Trim()
}

function Update-Self {
    if ($NoSelfUpdate -or -not $PSCommandPath) { return }
    $latest = ""
    try { $latest = (& $script:Ssh $SshHost "cat /workspaces/.devcontainer/win-agent.ps1") -join "`n" } catch { return }
    if (-not $latest) { return }

    $current = ""
    try { $current = Get-Content -Raw -LiteralPath $PSCommandPath } catch { }

    $hasBom = $false
    try {
        $head = [System.IO.File]::ReadAllBytes($PSCommandPath)
        $hasBom = ($head.Length -ge 3 -and $head[0] -eq 0xEF -and $head[1] -eq 0xBB -and $head[2] -eq 0xBF)
    } catch { }

    if ((Normalize-Script $latest) -eq (Normalize-Script $current) -and $hasBom) { return }

    Write-Host "win-agent.ps1を更新した。終了する(win-agent.batが起動し直す)"
    Set-Content -LiteralPath $PSCommandPath -Value $latest -Encoding UTF8
    # 自分では起動し直さない。win-agent.batのループが新しい内容で起動する。
    # (自前で子プロセスを起動する方式は、引数の渡し方を1つ間違えるだけで
    #  そこで全部止まるため採らない)
    exit 0
}

# --- パラメータの検証 --------------------------------------------------------
# コンテナ側から来る値をそのまま使わない。書式に合わないものは弾く。
function Get-Port([hashtable]$opt) {
    $p = [string]$opt["port"]
    if ($p -match '^COM\d{1,3}$') { return $p }
    return ""
}
function Get-Number([hashtable]$opt, [string]$key, [int]$fallback) {
    $v = [string]$opt[$key]
    if ($v -match '^\d{1,7}$') { return [int]$v }
    return $fallback
}

# --- 6つの操作 ---------------------------------------------------------------
# いずれも 2>&1 でstderrも拾う。捨てると失敗理由がコンテナ側に届かない。

# scpは新しい版ほど内部でSFTPを使うため、sshd側にSubsystem sftpが無いと
# "subsystem request failed"で落ちる。まず素で試し、駄目なら旧SCPプロトコル(-O)で再試行する。
function Invoke-Scp([string[]]$scpArgs) {
    $out = & $script:Scp @scpArgs 2>&1 | ForEach-Object { "$_" }
    $rc = $LASTEXITCODE
    $out
    if ($rc -ne 0 -and ("$out" -match "subsystem request failed")) {
        "SFTPが使えないので -O (旧SCPプロトコル) で再試行する"
        & $script:Scp @("-O") + $scpArgs 2>&1 | ForEach-Object { "$_" }
        $rc = $LASTEXITCODE
    }
    "rc=$rc"
}

function Invoke-Sync {
    Remove-Item -Recurse -Force "$WorkDir\build" -ErrorAction SilentlyContinue
    "[build]"
    Invoke-Scp @("-r", "${SshHost}:/workspaces/build/winflash/build", "$WorkDir")
    # win_flash.pyだけはステージング済みのコピーではなく元のファイルから取る。
    # ステージし忘れると古い版が送られ、直したはずの不具合が再現する。
    "[win_flash.py]"
    Invoke-Scp @("${SshHost}:/workspaces/tools/win_flash.py", "$WorkDir")
    if (-not (Test-Path "$WorkDir\lib\esptool")) {
        "[lib]"
        Invoke-Scp @("-r", "${SshHost}:/workspaces/build/winflash/lib", "$WorkDir")
    }
    "[取得結果]"
    Get-ChildItem -Recurse -File "$WorkDir\build" -ErrorAction SilentlyContinue |
        ForEach-Object { "  {0,10}  {1}" -f $_.Length, $_.FullName }
}

# 外部コマンドのstderrをそのまま流すとPowerShellがErrorRecordとして
# "NativeCommandError"の枠付きで出力するため、文字列へ落としてから返す。
function Invoke-Ports {
    & $script:Py -m serial.tools.list_ports -v 2>&1 | ForEach-Object { "$_" }
}

function Invoke-Probe([string]$port) {
    & $script:Py -m esptool --chip esp32s3 --port $port chip_id 2>&1 | ForEach-Object { "$_" }
}

function Invoke-Reset([string]$port) {
    & $script:Py -m esptool --chip esp32s3 --port $port run 2>&1 | ForEach-Object { "$_" }
}

function Invoke-Flash([string]$port, [int]$baud) {
    & $script:Py "$WorkDir\win_flash.py" flash --port $port --baud $baud `
        --ssh $SshHost --workdir $WorkDir 2>&1 | ForEach-Object { "$_" }
}

function Invoke-Monitor([string]$port, [int]$sec) {
    & $script:Py "$WorkDir\win_flash.py" monitor --port $port --baud 115200 `
        --seconds $sec --ssh $SshHost --workdir $WorkDir 2>&1 | ForEach-Object { "$_" }
}

# --- 本体 --------------------------------------------------------------------
$script:Ssh = if ($Ssh) { $Ssh } else { Resolve-Exe "ssh" }
$script:Scp = if ($Scp) { $Scp } else { Resolve-Exe "scp" }
$script:Py = Resolve-Python

if (-not $script:Ssh -or -not $script:Scp) {
    Write-Host "エラー: ssh/scpが見つからない。"
    if (-not $script:Ssh) { Show-ExeSearch "ssh" }
    if (-not $script:Scp) { Show-ExeSearch "scp" }
    Write-Host "パスが分かっている場合は -Ssh と -Scp で明示すること。例:"
    Write-Host '  powershell -NoProfile -ExecutionPolicy Bypass -File .\win-agent.ps1 -Ssh "C:\Windows\System32\OpenSSH\ssh.exe" -Scp "C:\Windows\System32\OpenSSH\scp.exe"'
    exit 1
}
if (-not $script:Py) {
    Write-Host "エラー: Pythonの実体が見つからない。-Python の引数でパスを明示すること。"
    exit 1
}
$env:PYTHONDONTWRITEBYTECODE = "1"
$env:PYTHONPATH = "$WorkDir\lib"

Write-Host "win-agent: ssh=$($script:Ssh)"
Write-Host "win-agent: python=$($script:Py)"
Write-Host "win-agent: workdir=$WorkDir"
Write-Host "devcontainerからの指示を待つ(Ctrl+Cで終了)"

while ($true) {
    Update-Self
    try {
        # USB/IPの逆トンネル(-R 3240)は~/.ssh/configのRemoteForwardで張られるので、
        # ここでは指定しない(二重指定になるため)。
        & $script:Ssh -o ServerAliveInterval=30 $SshHost "bash /workspaces/tools/win-agent.sh" | ForEach-Object {
            $line = $_
            if ($line -like "AGENT READY*") { Write-Host $line; return }

            $id, $rest = $line -split "`t", 2
            if (-not $rest) { return }

            $tokens = $rest -split '\s+'
            $action = $tokens[0]
            $opt = @{}
            foreach ($t in $tokens) {
                if ($t -match '^(\w+)=(.*)$') { $opt[$Matches[1]] = $Matches[2] }
            }
            Write-Host ("[{0}] {1}" -f (Get-Date -Format HH:mm:ss), $rest)

            $port = Get-Port $opt
            $out = ""
            $rc = 1

            try {
                switch ($action) {
                    "restart" { throw "RESTART" }
                    "sync"    { $out = (Invoke-Sync | Out-String); $rc = $LASTEXITCODE }
                    "ports"   { $out = (Invoke-Ports | Out-String); $rc = $LASTEXITCODE }
                    "probe" {
                        if (-not $port) { $out = "エラー: portの書式が不正`n"; $rc = 2 }
                        else { $out = (Invoke-Probe $port | Out-String); $rc = $LASTEXITCODE }
                    }
                    "reset" {
                        if (-not $port) { $out = "エラー: portの書式が不正`n"; $rc = 2 }
                        else { $out = (Invoke-Reset $port | Out-String); $rc = $LASTEXITCODE }
                    }
                    "flash" {
                        if (-not $port) { $out = "エラー: portの書式が不正`n"; $rc = 2 }
                        else { $out = (Invoke-Flash $port (Get-Number $opt "baud" 921600) | Out-String); $rc = $LASTEXITCODE }
                    }
                    "monitor" {
                        if (-not $port) { $out = "エラー: portの書式が不正`n"; $rc = 2 }
                        else { $out = (Invoke-Monitor $port (Get-Number $opt "sec" 180) | Out-String); $rc = $LASTEXITCODE }
                    }
                    default { $out = "エラー: 未知の操作`n"; $rc = 2 }
                }
            } catch {
                if ("$_" -eq "RESTART") { throw }
                $out = "EXCEPTION: $_`n"
                $rc = 1
            }

            $out += "=== EXIT rc=$rc ==="
            $out | & $script:Ssh $SshHost "mkdir -p /workspaces/logs/win && cat > /workspaces/logs/win/$id.log"
        }
    } catch {
        Write-Host "sshが切れた/再起動する: $_"
    }
    Start-Sleep -Seconds 2
}
