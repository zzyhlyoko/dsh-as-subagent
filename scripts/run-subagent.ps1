<#
.SYNOPSIS
  把 dsh 当作出进程 subagent 调用一次（支持重试与模型覆盖），返回退出码、会话 id 与最终答案。

.DESCRIPTION
  任务经 -Task 传入（复杂/多行用 here-string）；内部固定加 --json 以抓取会话 id 与无损最终答案，
  推理/诊断走 stderr。-Provider/-Model/-ReasoningEffort 会生成临时 --patch 覆盖默认模型；
  -Retries 在失败（退出码非 0）时按 -RetryDelaySeconds 间隔重试。脚本以最后一次尝试的退出码退出。

.PARAMETER Task
  任务文本；省略则从管道读取。

.PARAMETER SessionId
  续接已持久化会话（--session-id）。

.PARAMETER Cwd
  运行 dsh 的工作目录（子会话 workspace 根）。

.PARAMETER Provider
  覆盖 provider 路由（默认 deepseek-official）。

.PARAMETER Model
  覆盖模型 id。与 -Provider 组合可指向第三方端点已注册的模型。

.PARAMETER ReasoningEffort
  思考等级：low / off / high / max（可选）。

.PARAMETER Patch
  额外 --patch 覆盖文件（可多个）。

.PARAMETER Retries
  失败时的总尝试次数（默认 1；网络波动等瞬态故障建议 5）。

.PARAMETER RetryDelaySeconds
  每次重试前等待的秒数（默认 2）。

.PARAMETER Raw
  只输出最终答案，不输出 JSON 摘要。

.EXAMPLE
  .\run-subagent.ps1 -Task "跑测试" -Cwd C:\repo
.EXAMPLE
  .\run-subagent.ps1 -Task "任务" -Model deepseek-chat -ReasoningEffort high
.EXAMPLE
  .\run-subagent.ps1 -Task "任务" -Retries 5
.EXAMPLE
  .\run-subagent.ps1 -Task "继续" -SessionId session-abc -Model deepseek-chat

.NOTES
  执行策略拦截时，先 Set-ExecutionPolicy -Scope Process Bypass -Force，
  或 pwsh -NoProfile -ExecutionPolicy Bypass -File run-subagent.ps1 -Task "..."。
#>
param(
  [Parameter(Position = 0)]
  [string]$Task,
  [string]$SessionId,
  [string]$Cwd,
  [string]$Provider,
  [string]$Model,
  [string]$ReasoningEffort,
  [string[]]$Patch,
  [int]$Retries = 1,
  [double]$RetryDelaySeconds = 2,
  [switch]$Raw
)

# 1. 任务文本：-Task 优先，否则读管道
if ([string]::IsNullOrWhiteSpace($Task)) {
  $Task = ($input | Out-String)
}
$Task = $Task.Trim()
if ($Task -eq '') {
  Write-Error 'run-subagent: 任务为空（用 -Task 传，或经管道传入）' -ErrorAction Continue
  exit 2
}

# 2. 校验 dsh 可用
if (-not (Get-Command dsh -ErrorAction SilentlyContinue)) {
  Write-Error 'run-subagent: 找不到 dsh 命令（PATH 上无 dsh）' -ErrorAction Continue
  exit 2
}

# 3. 参数校验
if ($Retries -lt 1) { $Retries = 1 }
if ($RetryDelaySeconds -lt 0) { $RetryDelaySeconds = 0 }
if ($ReasoningEffort -and ($ReasoningEffort -notin @('low', 'off', 'high', 'max'))) {
  Write-Error "run-subagent: ReasoningEffort 合法取值 low/off/high/max，收到 $ReasoningEffort" -ErrorAction Continue
  exit 2
}

# 4. 工作目录
$pushed = $false
if ($Cwd) {
  if (-not (Test-Path -LiteralPath $Cwd -PathType Container)) {
    Write-Error "run-subagent: 目录不存在或不是目录：$Cwd" -ErrorAction Continue
    exit 2
  }
  Push-Location -LiteralPath $Cwd
  $pushed = $true
}

# 5. 生成模型覆盖 patch（临时文件；仅当指定了模型相关参数时）
#    agent-default-model 的 provider 与 model 都是必填，且 --patch 的 config 是整体替换，
#    所以这里必须同时写出 provider 和 model（缺省 deepseek-official / deepseek-flash）。
$tmpPatch = $null
if ($Provider -or $Model -or $ReasoningEffort) {
  $prov = if ($Provider) { $Provider } else { 'deepseek-official' }
  $model = if ($Model) { $Model } else { 'deepseek-flash' }
  $patchLines = @('- id: agent-default-model', '  config:', "    provider: $prov", "    model: $model")
  if ($ReasoningEffort) { $patchLines += "    reasoningEffort: $ReasoningEffort" }
  $tmpPatch = Join-Path $env:TEMP ("dsh-subagent-model-" + [guid]::NewGuid().ToString('N') + '.yml')
  $patchLines | Set-Content -LiteralPath $tmpPatch -Encoding UTF8
}

try {
  # 6. 组装参数。顺序：启动器参数（--profile、--patch）必须在
  #    应用参数（--json、--session-id、'-'）之前。
  $dshArgs = @('--profile', 'headless')
  foreach ($p in $Patch) { $dshArgs += @('--patch', $p) }
  if ($tmpPatch) { $dshArgs += @('--patch', $tmpPatch) }
  $dshArgs += @('--json')
  if ($SessionId) { $dshArgs += @('--session-id', $SessionId) }
  $dshArgs += '-'

  # 7. 重试循环：失败（退出码非 0）时按间隔重试，退出码 0 即提前结束
  $runSessionId = $null
  $runAnswer = $null
  $runError = $null
  $runReason = $null
  $exitCode = 1
  for ($attempt = 1; $attempt -le $Retries; $attempt++) {
    $runSessionId = $null
    $runAnswer = $null
    $runError = $null
    $runReason = $null

    # 任务经 stdin 传入；stdout（NDJSON 事件流）被捕获，stderr 透传。
    $lines = $Task | & dsh @dshArgs
    $exitCode = $LASTEXITCODE
    if ($null -eq $exitCode) { $exitCode = 1 }

    # 解析 NDJSON 事件流
    foreach ($line in $lines) {
      if ($null -eq $line -or "$line".Trim() -eq '') { continue }
      try { $ev = $line | ConvertFrom-Json } catch { continue }
      switch ($ev.type) {
        'session' { $runSessionId = $ev.sessionId }
        'final'   { $runAnswer = $ev.text }
        'error'   { $runError = $ev.message }
        'status'  { if ($ev.phase -eq 'turn_end' -and $null -ne $ev.reason) { $runReason = $ev.reason } }
      }
    }

    if ($exitCode -eq 0) { break }
    if ($attempt -lt $Retries) {
      Write-Error "run-subagent: 第 $attempt/$Retries 次尝试失败（exit=$exitCode），$RetryDelaySeconds 秒后重试" -ErrorAction Continue
      Start-Sleep -Seconds $RetryDelaySeconds
    }
  }

  # 8. 输出
  if ($Raw) {
    if ($null -ne $runAnswer) { Write-Output $runAnswer }
  } else {
    [ordered]@{
      exitCode = $exitCode
      sessionId = $runSessionId
      answer = $runAnswer
      reason = $runReason
      error = $runError
    } | ConvertTo-Json
  }
  exit $exitCode
} finally {
  if ($tmpPatch) { Remove-Item -LiteralPath $tmpPatch -Force -ErrorAction SilentlyContinue }
  if ($pushed) { Pop-Location }
}