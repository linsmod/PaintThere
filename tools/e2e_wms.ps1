<#
e2e_wms.ps1 - WMS 四进程端到端验收（vm_wms + test_wms_app + test_wms_shell + test_wms_rdp）

来源：wmwms 仓库的 scripts/run_e2e.ps1，迁入本项目后只改了产物名、构建目录与
日志落点，断言与场景未改。

为什么要有这个脚本：
  单测只能证明编解码自洽。真正容易坏的是 IPC 生命周期、内容通道、帧下发、
  进程退出清理，那些只有起进程才看得见。所以每个场景用独立管道名，互不干扰。

覆盖场景：
  proto   协议单测与进程身份单测（纯内存 / 真实句柄，不开服务）
  basic   APP 建窗+回推内容 / SHELL 声明工作区+切主题+枚举+点输入 / RDP 收帧
  crash   APP 被强杀 → 服务端靠进程句柄发现对端死亡并回收窗口
  restart SHELL 退出后窗口保留，新 SHELL 重连照样能枚举（外壳重启场景）

用法：
  pwsh tools/e2e_wms.ps1 [-SkipBuild] [-AppSeconds 8] [-KeepLogs]

退出码：0 全通过，1 有断言失败（日志留在 temp/e2e-wms-logs）。
#>
param(
    [switch]$SkipBuild,
    [int]$AppSeconds   = 8,
    [int]$ShellSeconds = 6,
    [int]$RdpSeconds   = 5,
    [switch]$KeepLogs
)

$ErrorActionPreference = 'Continue'

$RepoRoot = Split-Path -Parent $PSScriptRoot
$BuildDir = Join-Path $RepoRoot 'vm-desktop\build'
$LogDir   = Join-Path $RepoRoot 'temp\e2e-wms-logs'

if (Test-Path $LogDir) { Remove-Item $LogDir -Recurse -Force }
New-Item -ItemType Directory -Force $LogDir | Out-Null

$script:Fails  = 0
$script:Checks = 0

function Assert-Log {
    param([string]$File, [string]$Pattern, [string]$Name)
    $script:Checks++
    if (-not (Test-Path $File)) {
        $script:Fails++
        Write-Host ("  [FAIL] {0}: 日志缺失 {1}" -f $Name, $File) -ForegroundColor Red
        return
    }
    if (Select-String -Path $File -Pattern $Pattern -Quiet) {
        Write-Host ("  [ OK ] {0}" -f $Name) -ForegroundColor Green
    } else {
        $script:Fails++
        Write-Host ("  [FAIL] {0}: 未匹配 '{1}' in {2}" -f $Name, $Pattern, (Split-Path -Leaf $File)) -ForegroundColor Red
    }
}

function Assert-LogAny {
    param([string]$File, [string[]]$Patterns, [string]$Name)
    $script:Checks++
    if (-not (Test-Path $File)) {
        $script:Fails++
        Write-Host ("  [FAIL] {0}: 日志缺失" -f $Name) -ForegroundColor Red
        return
    }
    foreach ($p in $Patterns) {
        if (Select-String -Path $File -Pattern $p -Quiet) {
            Write-Host ("  [ OK ] {0}" -f $Name) -ForegroundColor Green
            return
        }
    }
    $script:Fails++
    Write-Host ("  [FAIL] {0}: 未匹配 {1}" -f $Name, ($Patterns -join ' | ')) -ForegroundColor Red
}

function Assert-NoLog {
    param([string]$File, [string]$Pattern, [string]$Name)
    $script:Checks++
    if ((Test-Path $File) -and (Select-String -Path $File -Pattern $Pattern -Quiet)) {
        $script:Fails++
        Write-Host ("  [FAIL] {0}: 不该出现 '{1}'" -f $Name, $Pattern) -ForegroundColor Red
    } else {
        Write-Host ("  [ OK ] {0}" -f $Name) -ForegroundColor Green
    }
}

function Get-Sse {
    # 下行通道要先拿门票：/events 与 /ws 都认 index 发的 boot 票，进程重启即失效。
    param([string]$Base, [string]$OutFile, [int]$Seconds = 4)
    $boot = (& curl.exe -s --noproxy '*' --max-time 4 "$Base/boot" 2>$null)
    if ($boot) { $boot = ($boot | Out-String).Trim() }
    & curl.exe -s --noproxy '*' --max-time $Seconds "$Base/events?boot=$boot" -o $OutFile 2>$null | Out-Null
}

function Start-Tool {
    param([string]$Exe, [string[]]$Arguments, [string]$OutFile, [string]$ErrFile)
    $path = Join-Path $BuildDir $Exe
    if (-not (Test-Path $path)) { throw "missing executable: $path" }
    return Start-Process -FilePath $path -ArgumentList $Arguments `
        -RedirectStandardOutput $OutFile -RedirectStandardError $ErrFile `
        -PassThru -NoNewWindow
}

function Wait-ServerReady {
    param([string]$ErrFile, [int]$TimeoutMs = 5000)
    $deadline = (Get-Date).AddMilliseconds($TimeoutMs)
    while ((Get-Date) -lt $deadline) {
        if ((Test-Path $ErrFile) -and (Select-String -Path $ErrFile -Pattern 'listening' -Quiet)) { return $true }
        Start-Sleep -Milliseconds 100
    }
    return $false
}

function Wait-Log {
    param([string]$File, [string]$Pattern, [int]$TimeoutMs = 6000)
    $deadline = (Get-Date).AddMilliseconds($TimeoutMs)
    while ((Get-Date) -lt $deadline) {
        if ((Test-Path $File) -and (Select-String -Path $File -Pattern $Pattern -Quiet)) { return $true }
        Start-Sleep -Milliseconds 100
    }
    return $false
}

function Stop-Tool {
    param($Proc)
    if ($null -eq $Proc) { return }
    try {
        if (-not $Proc.HasExited) { Stop-Process -Id $Proc.Id -Force }
        $Proc.WaitForExit(3000) | Out-Null
    } catch { }
}

function New-Pipe([string]$tag) {
    return '\\.\pipe\vmwm_e2e_' + $tag + '_' + (Get-Random -Maximum 100000)
}

# ---------------------------------------------------------------
# 0. 构建（-SkipBuild 跳过）
# ---------------------------------------------------------------
if (-not $SkipBuild) {
    Write-Host '== build ==' -ForegroundColor Cyan
    $cmake = 'C:\msys64\mingw64\bin\cmake.exe'
    if (-not (Test-Path $cmake)) { $cmake = (Get-Command cmake -ErrorAction SilentlyContinue).Source }
    if (-not $cmake) { Write-Host 'cmake not found' -ForegroundColor Red; exit 1 }
    & $cmake --build $BuildDir -j 8 2>&1 | Select-Object -Last 3
    if ($LASTEXITCODE -ne 0) { Write-Host 'build failed' -ForegroundColor Red; exit 1 }
}

# ---------------------------------------------------------------
# 1. 协议单测与进程身份单测
# ---------------------------------------------------------------
Write-Host "`n== scenario: proto ==" -ForegroundColor Cyan
& (Join-Path $BuildDir 'test_wms_ipc.exe') --probe | ForEach-Object { Write-Host "  $_" }
if ($LASTEXITCODE -ne 0) { $script:Fails++ }
$script:Checks++

& (Join-Path $BuildDir 'test_wms_peer.exe') --probe | ForEach-Object { Write-Host "  $_" }
if ($LASTEXITCODE -ne 0) { $script:Fails++ }
$script:Checks++

# ---------------------------------------------------------------
# 2. basic：APP + SHELL + RDP
# ---------------------------------------------------------------
Write-Host "`n== scenario: basic ==" -ForegroundColor Cyan
$pipe = New-Pipe 'basic'
$srvO = Join-Path $LogDir 'basic-server.out.txt'
$srvE = Join-Path $LogDir 'basic-server.err.txt'
$appO = Join-Path $LogDir 'basic-app.out.txt'
$appE = Join-Path $LogDir 'basic-app.err.txt'
$shO  = Join-Path $LogDir 'basic-shell.out.txt'
$shE  = Join-Path $LogDir 'basic-shell.err.txt'
$rdpO = Join-Path $LogDir 'basic-rdp.out.txt'
$rdpE = Join-Path $LogDir 'basic-rdp.err.txt'

$srv = Start-Tool 'vm_wms.exe' @('--pipe', $pipe, '--screen', '800x600') $srvO $srvE
if (-not (Wait-ServerReady $srvE)) { Write-Host '  server 未就绪' -ForegroundColor Red; $script:Fails++ }

$app = Start-Tool 'test_wms_app.exe' @('--pipe', $pipe, '--seconds', "$AppSeconds") $appO $appE
Start-Sleep -Milliseconds 400
$sh  = Start-Tool 'test_wms_shell.exe' @('--pipe', $pipe, '--seconds', "$ShellSeconds", '--theme', 'dark', '--click', '220,160', '--click2', '400,584', '--move', '400,300', '--focus') $shO $shE
Start-Sleep -Milliseconds 400
$rdpDump = Join-Path $LogDir 'basic-fullframe.json'
$rdp = Start-Tool 'test_wms_rdp.exe' @('--pipe', $pipe, '--seconds', "$RdpSeconds", '--dump', $rdpDump) $rdpO $rdpE

$app.WaitForExit(30000) | Out-Null
$sh.WaitForExit(30000)  | Out-Null
$rdp.WaitForExit(30000) | Out-Null
Start-Sleep -Milliseconds 400
Stop-Tool $srv

Assert-Log $srvE 'handshake client id=\d+ type=APP peer pid=\d+' '服务端识别 APP 进程身份'
Assert-Log $srvE 'handshake client id=\d+ type=SHELL'                  '服务端识别 SHELL'
Assert-Log $srvE 'handshake client id=\d+ type=RDP'                    '服务端识别 RDP'
Assert-Log $appO 'layout_req win=\d+ epoch=1'                          'APP 收到首个布局请求'
Assert-Log $appO 'push content .* accepted=1'                          '内容通道被采纳'
Assert-Log $appO 'input win=\d+ kind=1 down=1'                         'APP 收到转成局部坐标的点击'
Assert-Log $shO  'set_workarea 0,0 800x568 -> ok'                      'SHELL 声明工作区'
Assert-Log $shO  'list_windows -> 2'                                   'SHELL 枚举到 2 个窗口'
Assert-Log $shO  'set_focus \d+ -> ok'                                 'SHELL 切换焦点'
Assert-Log $shO  "id=\d+ 'Taskbar' z=\d+ flags=15"                     '任务栏窗口带 UNDECORATED/TOPMOST/PINNED/NOFOCUS'
Assert-Log $shO  'pointer x=400 y=300'                                 '指针位置广播到 SHELL（移出窗口也能清 hover）'
Assert-Log $shO  'input id=\d+ kind=1 down=1 \(400,16\)'               '点击任务栏投递给 SHELL 属主'
Assert-Log    $shO  'after #\d+ id=1 focused=1 flags=0'  '点击任务栏后应用仍是焦点窗口（NOFOCUS）'
Assert-NoLog  $shO  "Taskbar.*focused=1"                '任务栏从未成为焦点窗口'
Assert-Log $rdpO 'request_fullframe -> ok'                             'RDP fullframe 请求被受理'
Assert-Log $rdpO 'fullframe frame_id=\d+'                              'RDP 收到 fullframe 数据'
Assert-Log $rdpO 'frame frame_id=\d+'                                  'RDP 收到增量帧'
Assert-Log $rdpO "resources id='default'"                              'RDP 收到资源包应答'
Assert-NoLog $srvE 'bad frame'                                         '没有任何坏帧'
# 光有帧不够：帧里必须真的有应用推来的内容，否则可能只是装饰
Assert-Log $rdpDump 'app_rect_0'                                       'fullframe 含应用内容的绘制命令'
Assert-Log    $rdpDump 'w1_title'   '普通窗口带标题栏装饰'
Assert-NoLog  $rdpDump 'w2_title'   '任务栏无标题栏装饰（UNDECORATED 生效）'
Stop-Tool $app; Stop-Tool $sh; Stop-Tool $rdp

# ---------------------------------------------------------------
# 3. crash：APP 被强杀，服务端按进程句柄回收
# ---------------------------------------------------------------
Write-Host "`n== scenario: crash ==" -ForegroundColor Cyan
$pipe = New-Pipe 'crash'
$srvO = Join-Path $LogDir 'crash-server.out.txt'
$srvE = Join-Path $LogDir 'crash-server.err.txt'
$appO = Join-Path $LogDir 'crash-app.out.txt'
$appE = Join-Path $LogDir 'crash-app.err.txt'

$srv = Start-Tool 'vm_wms.exe' @('--pipe', $pipe) $srvO $srvE
Wait-ServerReady $srvE | Out-Null
$app = Start-Tool 'test_wms_app.exe' @('--pipe', $pipe, '--seconds', '60', '--no-anim') $appO $appE
Start-Sleep -Milliseconds 1500
Stop-Process -Id $app.Id -Force          # 模拟崩溃：不走 CloseWindow、不发 FIN
Start-Sleep -Milliseconds 2500           # 等 peer 探活定时器（1s 一次）
Stop-Tool $srv

# Windows 上进程被强杀时管道句柄会立刻关闭，服务端先收到 EOF；
# 进程句柄探活是备用路径（用于「句柄被别的进程继承、EOF 不来」的情况）。
Assert-LogAny $srvE @('peer process exited', 'end of file') '服务端检测到对端消失'
Assert-Log    $srvE 'remove window id=1'                    '死连接的窗口被回收'
Assert-Log    $srvE 'client id=\d+ \(APP\) disconnected'     '连接被清理并落日志'
Stop-Tool $app

# ---------------------------------------------------------------
# 4. restart：SHELL 退出后窗口保留，新 SHELL 重连可枚举
# ---------------------------------------------------------------
Write-Host "`n== scenario: restart ==" -ForegroundColor Cyan
$pipe  = New-Pipe 'restart'
$srvO  = Join-Path $LogDir 'restart-server.out.txt'
$srvE  = Join-Path $LogDir 'restart-server.err.txt'
$appO  = Join-Path $LogDir 'restart-app.out.txt'
$appE  = Join-Path $LogDir 'restart-app.err.txt'
$sh1O  = Join-Path $LogDir 'restart-shell1.out.txt'
$sh1E  = Join-Path $LogDir 'restart-shell1.err.txt'
$sh2O  = Join-Path $LogDir 'restart-shell2.out.txt'
$sh2E  = Join-Path $LogDir 'restart-shell2.err.txt'

$srv = Start-Tool 'vm_wms.exe' @('--pipe', $pipe) $srvO $srvE
Wait-ServerReady $srvE | Out-Null
$app = Start-Tool 'test_wms_app.exe' @('--pipe', $pipe, '--seconds', '30', '--no-anim') $appO $appE
Start-Sleep -Milliseconds 400
$sh1 = Start-Tool 'test_wms_shell.exe' @('--pipe', $pipe, '--seconds', '2', '--focus') $sh1O $sh1E
$sh1.WaitForExit(10000) | Out-Null          # 外壳退出
Start-Sleep -Milliseconds 300
$sh2 = Start-Tool 'test_wms_shell.exe' @('--pipe', $pipe, '--seconds', '2', '--focus') $sh2O $sh2E
$sh2.WaitForExit(10000) | Out-Null

Assert-Log $sh1O "id=1 'wmwm test window'"               '第一个 SHELL 枚举到 APP 的窗口'
Assert-Log $srvE 'client id=\d+ \(SHELL\) disconnected'  '外壳退出，服务端断开其连接'
Assert-Log $sh2O "id=1 'wmwm test window'"               '外壳重启后仍能枚举到同一个窗口（id 稳定）'
Assert-NoLog $srvE 'remove window id=1'                  '外壳退出没有连带回收 APP 的窗口（此时 APP 仍在）'
Stop-Tool $app; Stop-Tool $sh1; Stop-Tool $sh2
$app.WaitForExit(5000) | Out-Null
Stop-Tool $srv

# ---------------------------------------------------------------
# 5. rdp：呈现端服务器（HTTP / SSE / WebSocket / respack）
#    这一层是唯一对浏览器说话的进程：帧来自 EVT_FRAME / EVT_FULLFRAME，
#    输入经 rdp 转成 CMD_INPUT，资源包用缓存的 EVT_RESOURCES 应答。
# ---------------------------------------------------------------
Write-Host "`n== scenario: rdp ==" -ForegroundColor Cyan
$pipe    = New-Pipe 'rdp'
$rdpPort = 19000 + (Get-Random -Maximum 900)
$srvO  = Join-Path $LogDir 'rdp-server.out.txt'
$srvE  = Join-Path $LogDir 'rdp-server.err.txt'
$appO  = Join-Path $LogDir 'rdp-app.out.txt'
$appE  = Join-Path $LogDir 'rdp-app.err.txt'
$rdpO  = Join-Path $LogDir 'rdp-server1.out.txt'
$rdpE  = Join-Path $LogDir 'rdp-server1.err.txt'
$rdp2O = Join-Path $LogDir 'rdp-server2.out.txt'
$rdp2E = Join-Path $LogDir 'rdp-server2.err.txt'
$sseDump     = Join-Path $LogDir 'rdp-sse.txt'
$sse2Dump    = Join-Path $LogDir 'rdp-sse2.txt'
$idxDump     = Join-Path $LogDir 'rdp-index.html'
$respackDump = Join-Path $LogDir 'rdp-respack.bin'
$assetsDir   = Join-Path $RepoRoot 'assets'
$base        = "http://127.0.0.1:$rdpPort"

$srv = Start-Tool 'vm_wms.exe' @('--pipe', $pipe, '--screen', '800x600') $srvO $srvE
if (-not (Wait-ServerReady $srvE)) { Write-Host '  server 未就绪' -ForegroundColor Red; $script:Fails++ }
$app = Start-Tool 'test_wms_app.exe' @('--pipe', $pipe, '--seconds', '120', '--no-anim') $appO $appE
Start-Sleep -Milliseconds 800

$rdp = Start-Tool 'vm_rdp.exe' @('--port', "$rdpPort", '--pipe', $pipe, '--web-root', $assetsDir) $rdpO $rdpE
Wait-Log $rdpE "listening on 127.0.0.1:$rdpPort" | Out-Null
Start-Sleep -Milliseconds 800

& curl.exe -s --noproxy '*' --max-time 6 "$base/" -o $idxDump 2>$null | Out-Null
Get-Sse $base $sseDump 4
& curl.exe -s --noproxy '*' --max-time 6 "$base/respack/default" -o $respackDump 2>$null | Out-Null
# HTTP 回退通道（前端在 WS 不可用时用）
& curl.exe -s --noproxy '*' --max-time 6 -X POST -H "Content-Type: application/json" `
    -d '{"kind":"mousedown","x":220,"y":160,"button":0,"clientId":"e2e-http"}' "$base/input" 2>$null | Out-Null
# WebSocket 主路径
& python (Join-Path $RepoRoot 'tools\probe_ws_input.py') --port $rdpPort `
    --kind mousedown --x 200 --y 150 --client-id e2e-ws --quiet
$wsExit = $LASTEXITCODE
Start-Sleep -Milliseconds 500

Assert-Log $rdpE "listening on 127.0.0.1:$rdpPort" 'rdp 在独立端口监听'
Assert-Log $rdpE 'wms connected pipe='          'rdp 以 RDP 角色连上 vm_wms'
Assert-Log $idxDump 'vm_renderer.js'            '首页加载渲染入口'
Assert-Log $sseDump 'event: resources'          'SSE 首帧带资源包引用'
Assert-Log $sseDump 'event: fullframe'          'SSE 收到 fullframe 基线'
Assert-Log $sseDump 'app_rect_0'                'fullframe 含应用内容的绘制命令'
Assert-Log $sseDump '"setTile"'                 'fullframe 按瓦片切分'
Assert-Log $rdpE 'input kind=mousedown'         '输入经 rdp 转成线路协议'
Assert-Log $appO 'input win=\d+ kind=1 down=1'  '应用收到 body 局部坐标的输入'
Assert-Log $rdpE "respack request id='default' -> 200" 'respack 用缓存的资源包应答'
Assert-NoLog $rdpE 'too slow'                   '没有慢客户端被断开'

# respack 容器：magic + 无内联图片（拆进程后只有主题资源）
$script:Checks++
if (Test-Path $respackDump) {
    $rb = [System.IO.File]::ReadAllBytes($respackDump)
    if ($rb.Length -ge 12 -and
        [System.Text.Encoding]::ASCII.GetString($rb[0..3]) -eq 'VMRP' -and
        [BitConverter]::ToUInt32($rb, 8) -eq 0) {
        Write-Host '  [ OK ] respack 容器（VMRP，imgCount=0）' -ForegroundColor Green
    } else {
        $script:Fails++
        Write-Host '  [FAIL] respack 容器格式不符合前端 parseResPack' -ForegroundColor Red
    }
} else {
    $script:Fails++
    Write-Host '  [FAIL] respack 未落盘' -ForegroundColor Red
}

$script:Checks++
if ($wsExit -eq 0) {
    Write-Host '  [ OK ] WebSocket 上行通道（101 + 输入帧）' -ForegroundColor Green
} else {
    $script:Fails++
    Write-Host '  [FAIL] WebSocket 上行通道' -ForegroundColor Red
}

# kill rdp 后重启：画面靠新的 fullframe 恢复，应用无需重推内容
Stop-Tool $rdp
Start-Sleep -Milliseconds 400
$rdp2 = Start-Tool 'vm_rdp.exe' @('--port', "$rdpPort", '--pipe', $pipe, '--web-root', $assetsDir) $rdp2O $rdp2E
Wait-Log $rdp2E "listening on 127.0.0.1:$rdpPort" | Out-Null
Start-Sleep -Milliseconds 800
Get-Sse $base $sse2Dump 4
Assert-Log $sse2Dump 'app_rect_0' 'kill rdp 重启后靠 fullframe 恢复应用内容'

Stop-Tool $app; Stop-Tool $rdp2; Stop-Tool $srv

# ---------------------------------------------------------------
# 6. shell：真实桌面外壳进程（vm_shell）
#    任务栏由外壳建窗并推内容；kill 后任务栏消失、应用不受影响；
#    重启后条目重建。这是 M5.1 的核心验收。
# ---------------------------------------------------------------
Write-Host "`n== scenario: shell ==" -ForegroundColor Cyan
$pipe   = New-Pipe 'shell'
$shPort = 19900 + (Get-Random -Maximum 90)
$srvO   = Join-Path $LogDir 'shell-server.out.txt'
$srvE   = Join-Path $LogDir 'shell-server.err.txt'
$appO   = Join-Path $LogDir 'shell-app.out.txt'
$appE   = Join-Path $LogDir 'shell-app.err.txt'
$shO    = Join-Path $LogDir 'shell-shell.out.txt'
$shE    = Join-Path $LogDir 'shell-shell.err.txt'
$sh2O   = Join-Path $LogDir 'shell-shell2.out.txt'
$sh2E   = Join-Path $LogDir 'shell-shell2.err.txt'
$shRdpO = Join-Path $LogDir 'shell-rdp.out.txt'
$shRdpE = Join-Path $LogDir 'shell-rdp.err.txt'
$sseA   = Join-Path $LogDir 'shell-sse-before.txt'
$sseB   = Join-Path $LogDir 'shell-sse-killed.txt'
$sseC   = Join-Path $LogDir 'shell-sse-restarted.txt'
$shBase = "http://127.0.0.1:$shPort"
$fontPath = Join-Path $assetsDir 'font.ttf'

$srv = Start-Tool 'vm_wms.exe' @('--pipe', $pipe, '--screen', '800x600') $srvO $srvE
if (-not (Wait-ServerReady $srvE)) { Write-Host '  server 未就绪' -ForegroundColor Red; $script:Fails++ }
$app = Start-Tool 'test_wms_app.exe' @('--pipe', $pipe, '--seconds', '60', '--no-anim') $appO $appE
Start-Sleep -Milliseconds 400
$sh = Start-Tool 'vm_shell.exe' @('--pipe', $pipe, '--font', $fontPath) $shO $shE
Start-Sleep -Milliseconds 600
$shRdp = Start-Tool 'vm_rdp.exe' @('--port', "$shPort", '--pipe', $pipe,
    '--web-root', $assetsDir, '--font', $fontPath) $shRdpO $shRdpE
Wait-Log $shRdpE "listening on 127.0.0.1:$shPort" | Out-Null
Start-Sleep -Milliseconds 700

Get-Sse $shBase $sseA 4
Assert-Log    $srvE  'handshake client id=\d+ type=SHELL'          '真实外壳以 SHELL 角色连上'
Assert-Log    $shE   'push content count=\d+ accepted=1'           '外壳推送任务栏内容被采纳'
Assert-Log    $sseA  '开始'                                        'fullframe 含任务栏开始按钮'
Assert-Log    $sseA  'wmwm test win'                               '任务栏含应用条目'

Stop-Process -Id $sh.Id -Force
Start-Sleep -Milliseconds 800
Get-Sse $shBase $sseB 4
Assert-NoLog  $sseB  '开始'                                        'kill 外壳后任务栏消失'
Assert-Log    $sseB  'wmwm test window'                            'kill 外壳后应用内容保留'

$sh2 = Start-Tool 'vm_shell.exe' @('--pipe', $pipe, '--font', $fontPath) $sh2O $sh2E
Start-Sleep -Milliseconds 800
Get-Sse $shBase $sseC 4
Assert-Log    $sseC  '开始'                                        '重启外壳后任务栏重建'
Assert-Log    $sseC  'wmwm test win'                               '重启后任务栏条目重建'

Stop-Tool $app; Stop-Tool $sh2; Stop-Tool $shRdp; Stop-Tool $srv

# ---------------------------------------------------------------
# 7. app：新架构应用（vm_app，只依赖 VmCmdBuffer 与 wms 协议）
#    两个实例并发 → 内容进帧、任务栏两条目 → 点击命中 → kill 一个只掉自己的窗口。
# ---------------------------------------------------------------
Write-Host "`n== scenario: app ==" -ForegroundColor Cyan
$pipe    = New-Pipe 'app'
$appPort = 19800 + (Get-Random -Maximum 90)
$a6SrvO  = Join-Path $LogDir 'app-server.out.txt'
$a6SrvE  = Join-Path $LogDir 'app-server.err.txt'
$a6ShO   = Join-Path $LogDir 'app-shell.out.txt'
$a6ShE   = Join-Path $LogDir 'app-shell.err.txt'
$a6A1O   = Join-Path $LogDir 'app-a1.out.txt'
$a6A1E   = Join-Path $LogDir 'app-a1.err.txt'
$a6A2O   = Join-Path $LogDir 'app-a2.out.txt'
$a6A2E   = Join-Path $LogDir 'app-a2.err.txt'
$a6RdpO  = Join-Path $LogDir 'app-rdp.out.txt'
$a6RdpE  = Join-Path $LogDir 'app-rdp.err.txt'
$sse1    = Join-Path $LogDir 'app-sse-before.txt'
$sse2    = Join-Path $LogDir 'app-sse-after-kill.txt'
$a6Base  = "http://127.0.0.1:$appPort"

$srv = Start-Tool 'vm_wms.exe' @('--pipe', $pipe, '--screen', '800x600') $a6SrvO $a6SrvE
if (-not (Wait-ServerReady $a6SrvE)) { Write-Host '  server 未就绪' -ForegroundColor Red; $script:Fails++ }
$sh = Start-Tool 'vm_shell.exe' @('--pipe', $pipe, '--font', $fontPath) $a6ShO $a6ShE
Start-Sleep -Milliseconds 400
$a6App1 = Start-Tool 'vm_app.exe' @('--pipe', $pipe, '--title', 'Hello', '--x', '40', '--y', '60',
    '--font', $fontPath) $a6A1O $a6A1E
Start-Sleep -Milliseconds 500
$a6App2 = Start-Tool 'vm_app.exe' @('--pipe', $pipe, '--title', 'Second', '--x', '460', '--y', '120',
    '--font', $fontPath) $a6A2O $a6A2E
Start-Sleep -Milliseconds 500
$a6Rdp = Start-Tool 'vm_rdp.exe' @('--port', "$appPort", '--pipe', $pipe,
    '--web-root', $assetsDir, '--font', $fontPath) $a6RdpO $a6RdpE
Wait-Log $a6RdpE "listening on 127.0.0.1:$appPort" | Out-Null
Start-Sleep -Milliseconds 800

Get-Sse $a6Base $sse1 4
Assert-Log $a6SrvE 'handshake client id=\d+ type=APP'          '新 app 以 APP 角色连上'
Assert-Log $a6A1E 'layout epoch=1 body='                        'app 收到首次布局请求'
Assert-Log $a6A1E 'push epoch=1 count=\d+ accepted=1'          'app 内容被 wms 采纳'
Assert-Log $sse1  'vm_app / A1'                                 'fullframe 含 app 手写内容'
Assert-Log $sse1  'Second'                                      '任务栏含第二个 app 条目'

# 点击 Hello 的按钮（窗口 40,60 + body 偏移 1,30 + 按钮 16,56）
& curl.exe -s --noproxy '*' --max-time 4 -X POST -H "Content-Type: application/json" `
    -d '{"kind":"mousedown","x":120,"y":160,"button":0,"clientId":"e2e-app"}' "$a6Base/input" 2>$null | Out-Null
& curl.exe -s --noproxy '*' --max-time 4 -X POST -H "Content-Type: application/json" `
    -d '{"kind":"mouseup","x":120,"y":160,"button":0,"clientId":"e2e-app"}' "$a6Base/input" 2>$null | Out-Null
Start-Sleep -Milliseconds 500
Assert-Log $a6A1E 'button -> pressed=1 clicks=1'                '点击命中按钮并改状态'

Stop-Process -Id $a6App2.Id -Force
Start-Sleep -Milliseconds 700
Get-Sse $a6Base $sse2 4
Assert-NoLog $sse2 'Second'                                     'kill 第二个 app 后其窗口与条目消失'
Assert-Log   $sse2 'vm_app / A1'                                'kill 后另一个 app 内容保留'

Stop-Tool $a6App1; Stop-Tool $a6Rdp; Stop-Tool $sh; Stop-Tool $srv

# ---------------------------------------------------------------
# 汇总
# ---------------------------------------------------------------
$ok = $script:Checks - $script:Fails
Write-Host ("`n== {0}/{1} checks passed ==" -f $ok, $script:Checks) -ForegroundColor ($(if ($script:Fails) { 'Red' } else { 'Green' }))
if ($script:Fails) {
    Write-Host "日志：$LogDir" -ForegroundColor Yellow
    exit 1
}
if (-not $KeepLogs) { }
Write-Host "日志：$LogDir"
exit 0
