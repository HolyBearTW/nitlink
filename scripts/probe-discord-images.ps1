# Explicit, bounded live-client diagnostic. Uses its own PID and clears only
# that activity. No account/token data, uploads, or NitLink config writes.
param([string]$OutputPath = (Join-Path $PSScriptRoot '..\out\discord-image-probe.json'))
$ErrorActionPreference = 'Stop'
$taskReport = [ordered]@{ status='unavailable'; visualVerified=$false; probes=@(); cleanupAccepted=$false }
$taskPipe = $null
function Write-TaskFrame([int]$opcode, $payload) {
    $taskBytes = [Text.Encoding]::UTF8.GetBytes(($payload | ConvertTo-Json -Depth 12 -Compress))
    $taskHeader = [byte[]]::new(8)
    [BitConverter]::GetBytes($opcode).CopyTo($taskHeader, 0)
    [BitConverter]::GetBytes($taskBytes.Length).CopyTo($taskHeader, 4)
    $taskPipe.Write($taskHeader, 0, 8)
    $taskPipe.Write($taskBytes, 0, $taskBytes.Length)
    $taskPipe.Flush()
}
function Read-TaskBytes([int]$length) {
    $taskBuffer = [byte[]]::new($length)
    $taskOffset = 0
    while ($taskOffset -lt $length) {
        $taskRead = $taskPipe.ReadAsync($taskBuffer, $taskOffset, $length-$taskOffset)
        if (-not $taskRead.Wait(4000)) { throw 'IPC response timeout' }
        $taskCount = $taskRead.GetAwaiter().GetResult()
        if (-not $taskCount) { throw 'IPC stream closed' }
        $taskOffset += $taskCount
    }
    return ,$taskBuffer
}
function Read-TaskReply([string]$nonce) {
    for ($taskFrame=0; $taskFrame -lt 8; $taskFrame++) {
        $taskHeader = Read-TaskBytes 8
        $taskOpcode = [BitConverter]::ToInt32($taskHeader, 0)
        $taskLength = [BitConverter]::ToInt32($taskHeader, 4)
        if ($taskLength -lt 0 -or $taskLength -gt 65536) { throw 'Unbounded IPC frame' }
        $taskText = [Text.Encoding]::UTF8.GetString((Read-TaskBytes $taskLength))
        if ($taskOpcode -eq 3) {
            # Ping/Pong payloads must be echoed byte for byte.
            $taskHeader[0] = 4
            $taskPipe.Write($taskHeader, 0, 8)
            $taskBody = [Text.Encoding]::UTF8.GetBytes($taskText)
            $taskPipe.Write($taskBody, 0, $taskBody.Length)
            continue
        }
        if ($taskOpcode -ne 1) { throw "Unexpected IPC opcode $taskOpcode" }
        $taskReply = $taskText | ConvertFrom-Json
        if ((-not $nonce -and $taskReply.evt -eq 'READY') -or $taskReply.nonce -eq $nonce) { return $taskReply }
    }
    throw 'No matching RPC response within frame bound'
}
try {
    for ($taskIndex=0; $taskIndex -lt 10; $taskIndex++) {
        $taskPipe = [IO.Pipes.NamedPipeClientStream]::new('.', "discord-ipc-$taskIndex", [IO.Pipes.PipeDirection]::InOut, [IO.Pipes.PipeOptions]::Asynchronous)
        try { $taskPipe.Connect(250); break } catch { $taskPipe.Dispose(); $taskPipe=$null }
    }
    if (-not $taskPipe) { throw 'Discord IPC unavailable' }
    Write-TaskFrame 0 @{v=1; client_id='1505211372286246944'}
    $null = Read-TaskReply ''
    $taskStart = [DateTimeOffset]::UtcNow.ToUnixTimeSeconds()
    $taskImages = @(
        @{source='public-control'; url='https://cdn.discordapp.com/embed/avatars/0.png'},
        @{source='steamgriddb'; url='https://cdn2.steamgriddb.com/grid/72654158f52bf25f9a8101f78aceaa2f.jpg'},
        @{source='nintendo-tw'; url='https://www.nintendo.com/tw/switch/zelda-yakusai/img/product/package.jpg'},
        @{source='nintendo-tw-totk'; url='https://www.nintendo.com/tw/zelda/totk/products/img/soft/img__package.png'},
        @{source='nintendo-hk-eshop'; url='https://img-eshop.cdn.nintendo.net/i/0a48ca70f4e17da2a7ebd3b6033cf2a124975d6a697a9cd9e4e1fd8ec6fba9ee.jpg'},
        @{source='nintendo-hk-store'; url='https://store.nintendo.com.hk/media/catalog/product/cache/55351146f864384e20b16a2043f3a323/h/e/hero_nhl_v2.jpg'}
    )
    foreach ($taskImage in $taskImages) {
        foreach ($taskWithName in @($false, $true)) {
            $taskNonce = [Guid]::NewGuid().ToString('N')
            $taskActivity = @{details='NitLink image diagnostic'; state='Image compatibility check'; timestamps=@{start=$taskStart}; assets=@{large_image=$taskImage.url; large_text='NitLink image diagnostic'}}
            if ($taskWithName) { $taskActivity.name='Switch 2' }
            Write-TaskFrame 1 @{cmd='SET_ACTIVITY'; nonce=$taskNonce; args=@{pid=$PID; activity=$taskActivity}}
            $taskReply = Read-TaskReply $taskNonce
            $taskReturned = $taskReply.data.assets.large_image
            $taskReport.probes += [ordered]@{
                source=$taskImage.source; sentLargeImage=$taskImage.url; sentType='https-url'; withName=$taskWithName
                cmd=$taskReply.cmd; nonce=$taskReply.nonce; evt=$taskReply.evt
                ipcAccepted=($taskReply.cmd -eq 'SET_ACTIVITY' -and $taskReply.evt -ne 'ERROR')
                returnedImage=$taskReturned; returnedName=$taskReply.data.name
                errorCode=$taskReply.data.code; errorMessage=$taskReply.data.message
                requestedStart=$taskStart; returnedStart=$taskReply.data.timestamps.start
                proxyResolved=([string]$taskReturned).StartsWith('mp:external/'); visualVerified=$false
            }
            Start-Sleep -Milliseconds 2100
        }
    }
    $taskReport.status='complete'
} catch { $taskReport.reason=$_.Exception.Message }
finally {
    if ($taskPipe -and $taskPipe.IsConnected) {
        try {
            $taskNonce = [Guid]::NewGuid().ToString('N')
            Write-TaskFrame 1 @{cmd='SET_ACTIVITY'; nonce=$taskNonce; args=@{pid=$PID; activity=$null}}
            $taskReply = Read-TaskReply $taskNonce
            $taskReport.cleanupAccepted = $taskReply.evt -ne 'ERROR'
        } catch {}
    }
    if ($taskPipe) { $taskPipe.Dispose() }
    $taskParent = Split-Path -Parent ([IO.Path]::GetFullPath($OutputPath))
    $null = New-Item -ItemType Directory -Path $taskParent -Force
    $taskReport | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath $OutputPath -Encoding utf8
}
$taskReport | ConvertTo-Json -Depth 12
