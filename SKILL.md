---
name: dsh-as-subagent
description: 让外部 Agent（Codex CLI、Claude Code 等）把 dsh 当作「出进程 subagent」时使用：用 dsh --profile headless 从命令行唤起独立 dsh 进程跑自足任务，把委派工作从本 Agent 的前沿模型额度转移到 dsh 统一管理的其它 API，支持指定模型/思考等级、不可用时自动 fallback、会话续接、进展穿刺与强制终止。
whenToUse: 需要委派一个自足任务给独立 dsh 进程、且希望子任务跑在 dsh 配置的模型（而非本 Agent 的前沿模型）以省额度时；或本 Agent 没有内置 subagent 工具、只能通过命令行委派时。
---

# dsh-as-subagent：把 dsh 当作出进程 subagent

你（Codex CLI、Claude Code 等外部 Agent）默认 spawn 的 subagent 会复用你自己的前沿模型，进一步消耗有限额度。本技能让你用 `dsh --profile headless` 从命令行唤起一个**独立 dsh 进程**跑子任务，把它路由到 dsh 统一管理的任意模型上；dsh 无 GUI、不监听端口、跑完即退，天生适合被脚本/其它 agent 唤起。

调用器（二选一，下文统称 `$run`）：

- **`dsh-subagent.exe`（推荐）**：`.dsh\skills\dsh-as-subagent\scripts\dsh-subagent.exe`。原生 exe，直接 spawn `node.exe + bin.js`，**不需要 PowerShell**——没有执行策略、没有编码坑、没有 5.1/7 版本差异，外部 Agent 当普通命令调用即可。
- `run-subagent.ps1`：`.dsh\skills\dsh-as-subagent\scripts\run-subagent.ps1`（PowerShell 等价实现，跨平台备选；用 `-Task` 风格参数）。

两者都固定用 `--json` 抓取会话 id 与无损最终答案，返回 JSON 摘要 `{"exitCode","sessionId","answer","reason","error"}`，并以子进程退出码退出。

## 快速上手（三条命令）

```powershell
# 1) 唤起新会话（默认模型）
& $run --task "帮我查 X 并整理成表格"

# 2) 指定模型 + 思考等级
& $run --task "..." --model deepseek-chat --reasoning-effort high

# 3) 续接会话，并（按需）换模型
& $run --task "接着上一步继续" --session-id session-xxx --model deepseek-chat
```

## 1. 唤起新会话（subagent）

```powershell
$task = @'
<完整、自足的任务文本：把上下文和目标都写进去>
'@
& $run --task $task
```

- 子进程是全新会话，**不继承你（父 Agent）的任何对话**——任务必须自足。
- 返回 `exitCode`（0=完成，1=中止/出错/无轮次）、`sessionId`（供续接）、`answer`（最终答案）、`reason`/`error`（失败原因）。
- 长任务用后台作业跑（shell 的 run_in_background / timeout 机制），再取结果，别同步干等。

## 2. 深度管理：禁止无限递归

headless 子进程**自带** `subagent` / `subagent_fork` / `workflow` / `goal` 工具，因此必须由你把深度写进任务头部约束它。**默认 maxDepth = 1**（subagent 不能再唤起 subagent），用户可通过提示词调整。

每次派发时注入这段头部：

```text
【委派上下文】
- 你是被外部 Agent 委派的 dsh subagent。
- 当前深度：<d> / <maxDepth>。
- 若 <d> 已达到 <maxDepth>：你已到最大深度，禁止再使用 subagent / subagent_fork / workflow 工具唤起任何子代理，必须由你本人直接完成全部工作。
- 若 <d> < <maxDepth>：你最多还能向下再唤起 <maxDepth - d> 层子代理。

【任务】
<用户任务>
```

外部 Agent 自己维护深度计数：首次派发 d=1；若允许递归，每次往下派发前 d+1，达到 maxDepth 就不再往下派。

## 3. 指定 provider / model / 思考等级

```powershell
& $run --task "..." --provider deepseek-official --model deepseek-chat --reasoning-effort high
```

- `--provider`：路由名，默认 `deepseek-official`（DeepSeek API-key 路由）。
- `--model`：该 provider 下的模型 id。已知内置：`deepseek-flash`（默认，省额度首选）、`deepseek-chat`、`deepseek-v4-pro`；完整列表以 Web 模型页 / 官方 `/models` 列表为准。
- `--reasoning-effort`：`low` / `off` / `high` / `max`（可选）。

等价的底层 `--patch` 文件（**provider 与 model 都必填**，缺一启动即失败）：

```yaml
- id: agent-default-model
  config:
    provider: deepseek-official
    model: deepseek-chat
    reasoningEffort: high   # 可选
```

## 4. 模型不可用：fallback 阶梯

按顺序执行；**每发生一次 fallback（换模型/换 provider），都要明确告知用户**（试了哪些、为什么、现在用哪个）。

1. 用指定模型跑，**最多重试 5 次**（应对网络波动等瞬态故障）：
   ```powershell
   & $run --task $task --provider <p> --model <m1> --retries 5
   ```
2. 仍失败 → **同 provider 下换其它模型**，逐个重试（各 `--retries 5`）：
   ```powershell
   & $run --task $task --provider <p> --model <m2> --retries 5
   & $run --task $task --provider <p> --model <m3> --retries 5
   ```
3. 仍失败 → **切回 deepseek-official** 再重试：
   ```powershell
   & $run --task $task --model deepseek-chat --retries 5   # 或 deepseek-flash
   ```
4. 仍失败 → **放弃 dsh，改用你（外部 Agent）自己的 subagent 兜底**，并向用户说明原因。

判定「不可用」的信号：`exitCode` 非 0，且 `reason.error.code` 为 `MISSING_CREDENTIAL` / `MODEL_UNSUPPORTED` / `CLIENT_UPGRADE_REQUIRED` 或网络超时/5xx 等，或 stderr 有 `dsh: <code>: <message>` 行。

## 5. 续接会话 + 变更本次模型

```powershell
& $run --task "继续" --session-id <上次的sessionId> --model <新模型>
```

- 用第 1 次返回的 `sessionId` 续接；模型通过 `--model`（或 `--provider`/`--reasoning-effort`）在本次执行时重新读取，可临时更换。
- 约束：未知 id 会**在运行前失败**；会拒绝 subagent/fork 会话、记录在其它 cwd 的会话、跑在 headless 不组合的 preset 下的会话，以及本进程中已有存活 Agent 占用该 id 的会话。

## 6. 目标、进展穿刺、强制终止

- **设定目标**：把目标写进任务（子进程可用 `goal` 工具自行设定并推进）。
- **进展穿刺（首选 headless）**：后台跑 `$run` 时读它的 `--json` 事件流。事件是提交点，流式到达：
  `session`（携带 sessionId）、`status`（`turn_start`/`step_start`/`step_end`/`turn_end`，含 `reason`）、`text`、`thinking`、`tool_call`、`tool_result`、`final`。
- **进展穿刺（兜底，读历史/磁盘）**：无 headless 流或进程已退时，用本机已有的 `read-dsh-session` 技能解压会话日志
  `$env:USERPROFILE\.dsh\sessions\<工作区slug>\session-<id>\session.v4.jsonl.zstd`（7-Zip，或 `read-dsh-session\scripts\zstd-extract.mjs`）。
- **强制终止**：终止运行它的后台作业/进程即可（job_kill / `Stop-Process`）。终止后会话日志仍在磁盘，可继续用 zstd 兜底读最后进展。

## 7. 失败报告与优雅降级

- 子进程以任何方式中途失败：从返回 JSON 的 `exitCode` / `reason` / `error` 与 stderr 的 `dsh:` 行提取原因，**告知用户**。
- **不要因此撂挑子**：按第 4 节阶梯 fallback，最终兜底 = 你（外部 Agent）自己的 subagent；哪怕 dsh 完全不可用，也要把任务接回来完成或明确上报阻塞。

## 8. 调用器参数

`dsh-subagent.exe` 参数（`run-subagent.ps1` 用等价的 `-Task` / `-Model` / `-Retries` 风格）：

| 参数 | 默认 | 含义 |
|---|---|---|
| `--task <text>` | 位置参数/stdin | 任务文本（多行用变量传） |
| `--session-id <id>` | — | 续接已持久化会话 |
| `--cwd <dir>` | 当前目录 | 子会话 workspace 根 |
| `--provider <name>` | `deepseek-official` | provider 路由 |
| `--model <id>` | `deepseek-flash` | 模型 id（仅当给出模型相关参数时生效） |
| `--reasoning-effort <e>` | — | `low`/`off`/`high`/`max` |
| `--patch <file>` | — | 额外 `--patch` 文件（可重复） |
| `--retries <n>` | 1 | 失败重试总次数（网络波动建议 5） |
| `--retry-delay <sec>` | 2 | 重试间隔秒数 |
| `--raw` | — | 只输出最终答案，不输出 JSON |

## 9. 输出契约（基础）

- stdout = 最终答案（默认模式）或 `--json` 的 NDJSON 事件流；stderr = 流式推理（`dsh: reasoning:`）+ 诊断（`dsh: <code>: <message>`）。
- 退出码：0 = 完成；1 = 中止/出错/无轮次。`final.text` 是无损最终答案；`--json` 其余字符串/键上限 8 KiB。
- 凭据：headless **不用 GUI 保存的凭据**，API key 按「启动环境变量 → 运行目录 `.env` → `$DSH_HOME/.env`」回退；已导出为空的环境变量会遮蔽 `.env` 回退。

## 10. 已知限制与坑

- **每次调用只跑一个任务**：答完即退，无交互式追问；多步用 `--session-id` 串联。
- **`--patch` 的 `config` 是整体替换**：覆盖 `agent-default-model` 必须同时写 `provider` 和 `model`（见第 3 节）。
- **子进程执行一次就退出**：不能像内置 subagent 那样持续投喂消息，只能整段 `--session-id` 续接。
- **必须经 `dsh` 启动器运行**：绕过它会启动失败。
- **维护脚本时**：`run-subagent.ps1` 含中文注释，必须以 UTF-8 带 BOM 保存（PowerShell 5.1 中文区域默认按 GBK 读无 BOM 的 UTF-8，会把中文注释误读成乱码破坏解析），且注释里不要出现反引号。`dsh-subagent.exe` 无此问题；改了 `dsh-subagent.cs` 后重新编译：
  `C:\Windows\Microsoft.NET\Framework64\v4.0.30319\csc.exe /nologo /target:exe /out:dsh-subagent.exe /r:System.Web.Extensions.dll dsh-subagent.cs`
