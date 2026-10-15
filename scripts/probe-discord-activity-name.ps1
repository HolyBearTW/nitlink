# Bounded IPC compatibility probe for Activity.name and approved publisher art.
# Uses its own PID, clears that activity, and records no Discord account data.
$ErrorActionPreference='Stop'
$taskRoot=Split-Path -Parent $PSScriptRoot
$taskOut=Join-Path $taskRoot 'out\discord-artwork-probe.json'
New-Item -ItemType Directory -Path (Split-Path -Parent $taskOut) -Force | Out-Null
$taskPipe=[IO.Pipes.NamedPipeClientStream]::new('.', 'discord-ipc-0', [IO.Pipes.PipeDirection]::InOut, [IO.Pipes.PipeOptions]::Asynchronous)
$taskReport=[ordered]@{status='unavailable';accepted=$false;probes=@();visualVerified=$false}
function Write-TaskFrame([int]$opcode,$payload) {
    $taskBytes=[Text.Encoding]::UTF8.GetBytes(($payload|ConvertTo-Json -Depth 10 -Compress))
    $taskHeader=[byte[]]::new(8)
    [BitConverter]::GetBytes($opcode).CopyTo($taskHeader,0)
    [BitConverter]::GetBytes($taskBytes.Length).CopyTo($taskHeader,4)
    $taskPipe.Write($taskHeader,0,8);$taskPipe.Write($taskBytes,0,$taskBytes.Length);$taskPipe.Flush()
}
function Read-TaskBytes([int]$length) {
    $taskBuffer=[byte[]]::new($length);$taskOffset=0
    while($taskOffset -lt $length) {
        $taskRead=$taskPipe.ReadAsync($taskBuffer,$taskOffset,$length-$taskOffset)
        if(-not $taskRead.Wait(4000)){throw 'IPC response timeout'}
        $taskCount=$taskRead.GetAwaiter().GetResult();if(-not $taskCount){throw 'IPC stream closed'};$taskOffset+=$taskCount
    }
    return ,$taskBuffer
}
function Read-TaskFrame {
    $taskHeader=Read-TaskBytes 8
    $taskOpcode=[BitConverter]::ToInt32($taskHeader,0);$taskLength=[BitConverter]::ToInt32($taskHeader,4)
    if($taskLength -lt 0 -or $taskLength -gt 65536){throw 'Unbounded IPC frame'}
    $taskPayload=[Text.Encoding]::UTF8.GetString((Read-TaskBytes $taskLength))|ConvertFrom-Json
    return @{opcode=$taskOpcode;payload=$taskPayload}
}
try {
    $taskPipe.Connect(2000)
    Write-TaskFrame 0 @{v=1;client_id='1505211372286246944'}
    $taskReady=Read-TaskFrame
    if($taskReady.opcode -ne 1 -or $taskReady.payload.evt -ne 'READY'){throw 'Discord did not complete READY handshake'}
    $taskStart=[DateTimeOffset]::UtcNow.ToUnixTimeSeconds()
    $taskSeeds=Get-Content -LiteralPath (Join-Path $taskRoot 'official-artwork.json') -Raw -Encoding utf8 | ConvertFrom-Json
    foreach($taskGame in $taskSeeds.games) {
      foreach($taskArtwork in $taskGame.artworkSources) {
        $taskName='Nintendo Switch 2'
        $taskUrl=$taskArtwork.url
        $taskNonce=[Guid]::NewGuid().ToString('N')
        Write-TaskFrame 1 @{cmd='SET_ACTIVITY';nonce=$taskNonce;args=@{pid=$PID;activity=@{
            name=$taskName;details=$taskGame.name;state='NitLink official artwork compatibility check';timestamps=@{start=$taskStart};assets=@{large_image=$taskUrl}
        }}}
        $taskReply=Read-TaskFrame
        $taskAccepted=$taskReply.opcode -eq 1 -and $taskReply.payload.cmd -eq 'SET_ACTIVITY' -and $taskReply.payload.nonce -eq $taskNonce -and $taskReply.payload.evt -ne 'ERROR' -and $taskReply.payload.data.name -eq $taskName
        $taskReturnedImage=$taskReply.payload.data.assets.large_image
        $taskImageAccepted=$taskAccepted -and ($taskReturnedImage -eq $taskUrl -or $taskReturnedImage -like 'mp:external/*')
        $taskReport.probes+=@{gameId=$taskGame.id;url=$taskUrl;requestedName=$taskName;returnedName=$taskReply.payload.data.name;returnedImage=$taskReturnedImage;event=$taskReply.payload.evt;accepted=$taskAccepted;imageAccepted=$taskImageAccepted;start=$taskReply.payload.data.timestamps.start;requestedStart=$taskStart;errorCode=$taskReply.payload.data.code}
        Start-Sleep -Seconds 2
      }
    }
    $taskReport.accepted=@($taskReport.probes|Where-Object {-not $_.accepted -or -not $_.imageAccepted}).Count -eq 0
    $taskReport.status=if($taskReport.accepted){'accepted'}else{'unsupported'}
} catch { $taskReport.status='unavailable';$taskReport.reason=$_.Exception.Message }
finally {
    if($taskPipe.IsConnected) {
        try {Write-TaskFrame 1 @{cmd='SET_ACTIVITY';nonce=[Guid]::NewGuid().ToString('N');args=@{pid=$PID;activity=$null}}} catch {}
    }
    $taskPipe.Dispose()
    $taskReport | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $taskOut -Encoding utf8
}
$taskReport | ConvertTo-Json -Depth 8
