---
name: dsh-as-subagent
description: 让外部 Agent（Codex CLI、Claude Code 等）把 dsh 当作「出进程 subagent」时使用：用 dsh --profile headless 从命令行唤起独立 dsh 进程跑自足任务，把委派工作从本 Agent 的前沿模型额度转移到 dsh 统一管理的其它 API，支持指定模型/思考等级、不可用时自动 fallback、会话续接、进展穿刺与强制终止。
whenToUse: 需要委派一个自足任务给独立 dsh 进程、且希望子任务跑在 dsh 配置的模型（而非本 Agent 的前沿模型）以省额度时；或本 Agent 没有内置 subagent 工具、只能通过命令行委派时。
---

# dsh-as-subagent：把 dsh 当作出进程 subagent

你（Codex CLI、Claude Code 等外部 Agent）默认 spawn 的 subagent 会复用你自己的前沿模型，进一步消耗有限额度。本技能让你用 `dsh --profile headless` 从命令行唤起一个**独立 dsh 进程**跑子任务，把它路由到 dsh 统一管理的任意模型上；dsh 无 GUI、不监听端口、跑完即退，天生适合被脚本/其它 agent 唤起。

调用器是 `dsh-subagent.exe`（下文称 `$run`），路径 `.dsh\skills\dsh-as-subagent\scripts\dsh-subagent.exe`。它是原生 exe，直接 spawn `node.exe + bin.js`。不需要 PowerShell，也不需要 .NET 或 VC++ 运行库；动态链接的只有 Windows 10 及更新版本自带的 `KERNEL32.dll` 和 Universal CRT。外部 Agent 当普通命令调用即可。

它固定用 `--json` 抓取会话 id 与无损最终答案，返回 JSON 摘要 `{"exitCode","sessionId","answer","reason","error"}`，并以子进程退出码退出。

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
- `--model`：该 provider 下的模型 id。一个模型必须由 `provider` + `model` 一起确定。已知内置：`deepseek-flash`（默认，省额度首选）、`deepseek-chat`、`deepseek-v4-pro`；完整列表以 Web 模型页 / 官方 `/models` 列表为准。要按顺序尝试多个模型，用第 4 节的模型池，不要只重复写 `--model`。
- `--reasoning-effort`：`low` / `off` / `high` / `max`。它只作用于 `--provider`/`--model` 这一项。留空、纯空白，或字面量 `default`，都表示不填：patch 里不写 `reasoningEffort`。dsh 见到字段缺失时用该模型的 adapter 默认等级（`requested ?? defaultEffort`），不会把字符串 `default` 当成等级 id。模型池里每一项的思考等级写在该项自己的第三段，不继承这个参数。

等价的底层 `--patch` 文件（**provider 与 model 都必填**，缺一启动即失败）：

```yaml
- id: agent-default-model
  config:
    provider: deepseek-official
    model: deepseek-chat
    reasoningEffort: high   # 留空则整行不写，沿用模型默认等级
```

## 4. 模型池与失败回退

一个模型永远是 `provider` + `model`。思考等级不参与区分模型，只是这一项的可选附加。池里的一项写成 `provider/model` 或 `provider/model/effort`，例如 `[providerA/model1/high, providerA/model2, providerB/model3, provider/model666/low]`。第三段只能是 `low` / `off` / `high` / `max`；不写、留空或写 `default` 都表示这一项不填 `reasoningEffort`。

模型池是「指定模型」的扩展。一次调用把要尝试的模型按顺序交给调用器，调用器自己跑完整个池：

```powershell
& $run --task $task --retries 5 `
  --model-pool providerA/model1/high `
  --model-pool providerA/model2 `
  --model-pool providerB/model3 `
  --model-pool provider/model666/low
```

也可以先用 `--provider` 和 `--model`（以及可选的 `--reasoning-effort`）写第一项，再用重复的 `--model-pool` 追加后面的项。`--reasoning-effort` 不会套到没写第三段的池项上。

1. 从第一项开始。失败后只重试**当前项**，次数是 `--retries`（网络波动建议 5）。重试还没耗尽时不换模型。
2. 当前项的重试全部失败后，才换池中的下一项。这是池内切换，**不是失败回退**。stderr 有一行 `dsh-subagent: model pool switch ...`。告诉用户上一项在重试后失败、现在改用池里的下一项，但警告等级要低于回退：不要把它叫作 fallback，也不要建议放弃 dsh。
3. 整个池都失败后，才发生回退。直接改用 `deepseek-official`，并按平常的回退警告告诉用户（试过哪些、为什么、现在用哪个）。优先 `--provider deepseek-official --model deepseek-flash`；这个组合已经在池里失败过时，改用 `--model deepseek-chat`；两个都试过就跳过这一步。回退这一次同样带 `--retries 5`。
4. 这一步再失败 → **放弃 dsh，改用你（外部 Agent）自己的 subagent 兜底**，并向用户说明原因。

只指定了一个模型时，它就是只有一项的池：重试耗尽后直接进入第 3 步。不要自行猜测同一 provider 下的其它模型；用户要按顺序尝试多个模型，就把它们放进模型池。

判定某一项「失败」的信号：该次执行 `exitCode` 非 0，且 `reason.error.code` 为 `MISSING_CREDENTIAL` / `MODEL_UNSUPPORTED` / `CLIENT_UPGRADE_REQUIRED` 或网络超时/5xx 等，或 stderr 有 `dsh: <code>: <message>` 行。`model pool switch` 行不是这个信号。

## 5. 续接会话 + 变更本次模型

```powershell
& $run --task "继续" --session-id <上次的sessionId> --model <新模型>
```

- 用第 1 次返回的 `sessionId` 续接；模型通过 `--model`（或 `--provider`/`--reasoning-effort`）在本次执行时重新读取，可临时更换。
- 约束：未知 id 会**在运行前失败**；会拒绝 subagent/fork 会话、记录在其它 cwd 的会话、跑在 headless 不组合的 preset 下的会话，以及本进程中已有存活 Agent 占用该 id 的会话。

## 6. 目标、进展穿刺、强制终止

- **设定目标**：把目标写进任务（子进程可用 `goal` 工具自行设定并推进）。
- **进展穿刺（首选 headless）**：调用器自己消费 `--json` 事件流，stdout 只交回第 9 节的 JSON 摘要。运行中的诊断在 stderr（`dsh: reasoning:` 与 `dsh: <code>: <message>`）。事件本身仍是提交点：`session`（携带 sessionId）、`status`（`turn_start`/`step_start`/`step_end`/`turn_end`，含 `reason`）、`text`、`thinking`、`tool_call`、`tool_result`、`final`。
- **进展穿刺（兜底，读历史/磁盘）**：无 headless 流或进程已退时，用本机已有的 `read-dsh-session` 技能解压会话日志
  `$env:USERPROFILE\.dsh\sessions\<工作区slug>\session-<id>\session.v4.jsonl.zstd`（7-Zip，或 `read-dsh-session\scripts\zstd-extract.mjs`）。
- **强制终止**：终止运行它的后台作业/进程即可（job_kill / `Stop-Process`）。终止后会话日志仍在磁盘，可继续用 zstd 兜底读最后进展。

## 7. 失败报告与优雅降级

- 子进程以任何方式中途失败：从返回 JSON 的 `exitCode` / `reason` / `error` 与 stderr 的 `dsh:` 行提取原因，**告知用户**。stderr 里的 `model pool switch` 只是池内换下一项，按第 4 节的较低警告转告，不要当成失败回退。
- **不要因此撂挑子**：按第 4 节阶梯 fallback，最终兜底 = 你（外部 Agent）自己的 subagent；哪怕 dsh 完全不可用，也要把任务接回来完成或明确上报阻塞。

## 8. 调用器参数

`dsh-subagent.exe` 参数：

| 参数 | 默认 | 含义 |
|---|---|---|
| `--task <text>` | 位置参数/stdin | 任务文本（多行用变量传） |
| `--session-id <id>` | — | 续接已持久化会话 |
| `--cwd <dir>` | 当前目录 | 子会话 workspace 根 |
| `--provider <name>` | `deepseek-official` | provider 路由 |
| `--model <id>` | `deepseek-flash` | 模型 id。和 `--provider` 一起才能确定一个模型 |
| `--model-pool <provider>/<model>[/<effort>]` | — | 模型池的一项，可重复。顺序即尝试顺序；第三段只作用于该项 |
| `--reasoning-effort <e>` | 不填 | 只作用于 `--provider`/`--model`。`low`/`off`/`high`/`max`；空或 `default` 表示沿用模型默认等级 |
| `--patch <file>` | — | 额外 `--patch` 文件（可重复） |
| `--retries <n>` | 1 | 失败重试总次数（网络波动建议 5） |
| `--retry-delay <sec>` | 2 | 重试间隔秒数 |
| `--raw` | — | 只输出最终答案，不输出 JSON |
| `--version` | — | 打印 `dsh-subagent` 加 `VERSION` 文件里的版本号，然后退出 |

## 9. 输出契约（基础）

- 默认 stdout 是一行 JSON 摘要 `{"exitCode","sessionId","answer","reason","error"}`。`--raw` 只打印最终答案再加一个换行；没有答案时什么都不打印。stderr 仍是 dsh 的流式推理（`dsh: reasoning:`）和诊断（`dsh: <code>: <message>`）。
- 退出码：0 = 完成；1 = 中止/出错/无轮次；参数或环境不合法时调用器自己返回 2。`answer` 来自最后一条 `final.text`，无损。dsh 的 `--json` 里其余字符串/键上限 8 KiB。
- 凭据：headless **不用 GUI 保存的凭据**，API key 按「启动环境变量 → 运行目录 `.env` → `$DSH_HOME/.env`」回退；已导出为空的环境变量会遮蔽 `.env` 回退。

## 10. 已知限制与坑

- **每次调用只跑一个任务**：答完即退，无交互式追问；多步用 `--session-id` 串联。
- **`--patch` 的 `config` 是整体替换**：覆盖 `agent-default-model` 必须同时写 `provider` 和 `model`（见第 3 节）。
- **子进程执行一次就退出**：不能像内置 subagent 那样持续投喂消息，只能整段 `--session-id` 续接。
- **必须经 `dsh` 启动器运行**：绕过它会启动失败。
- **调用器是单个原生 exe**：重定向时 stdin/stdout/stderr 按 UTF-8 读写；接到控制台时用 `WriteConsoleW`，中文可以正常显示。改了 `scripts/dsh-subagent.c` 后，在仓库根执行 `tests\run-smoke.cmd`（它会先跑 `scripts\build.cmd`）。手工编译：
  `gcc -municode -O2 -s -Wall -Wextra -static-libgcc -o dsh-subagent.exe dsh-subagent.c`
  不要加 `-static`，也不要链接 `vcruntime`、`msvcp` 或 MinGW 的 `libwinpthread`。成品只应导入 `KERNEL32.dll` 和 `api-ms-win-crt-*.dll`。
