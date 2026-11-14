# dsh-as-subagent

把 [dsh](https://github.com/deepseek-ai/deepseek-harness) 当作**出进程 subagent** 用的技能。外部 Agent（Codex CLI、Claude Code，以及没有内置 subagent 工具的命令行 Agent）用它唤起一个独立的 `dsh --profile headless` 进程，把自足任务交出去，跑在 dsh 自己配置的模型上。

子进程没有界面、不监听端口，答完即退出。调用器抓住 `--json` 事件流，整理成一份 JSON 摘要再返回。

给 Agent 执行时看的完整约定在 [`SKILL.md`](SKILL.md)。本文是仓库说明。

## 仓库内容

| 路径 | 作用 |
|---|---|
| `SKILL.md` | 技能正文：何时委派、深度限制、模型 fallback、会话续接、失败怎么报 |
| `scripts/dsh-subagent.exe` | 调用器。直接启动 `node.exe` + `bin.js` |
| `scripts/dsh-subagent.c` | 调用器源码 |
| `scripts/build.cmd` | 用 MinGW-w64 UCRT gcc 重新编译 exe |
| `tests/run-smoke.cmd` | 编译并跑冒烟测试 |
| `VERSION` | 当前版本号。`scripts\build.cmd` 把它编进 exe |

`ref_repos/` 只在本地放参考检出，已被 `.gitignore` 排除，不会进入本仓库。

## 前置条件

- 本机 `PATH` 上能找到 `dsh`。`dsh-subagent.exe` 会在同一目录里找 `node.exe` 和 `node_modules/@deepseek-ai/dsh/lib/bin.js`。
- headless **不使用** GUI 里保存的凭据。API key 按「启动环境变量 → 运行目录 `.env` → `$DSH_HOME/.env`」回退。已经导出为空字符串的环境变量会挡住 `.env`。
- `dsh-subagent.exe` 是 Windows x64 程序，面向 Windows 10 及更新版本。它只动态链接系统自带的 `KERNEL32.dll` 和 Universal CRT，不依赖 .NET，也不依赖 VC++ 运行库。

## 安装

把本仓库放到 Agent 读取技能的目录，文件夹名保持 `dsh-as-subagent`。技能里引用的路径是：

```text
.dsh\skills\dsh-as-subagent\scripts\dsh-subagent.exe
```

## 调用

下面把这个 exe 写成 `$run`。

```powershell
# 新会话，默认模型 deepseek-flash
& $run --task "帮我查 X 并整理成表格"

# 指定路由、模型与思考等级
& $run --task "..." --provider deepseek-official --model deepseek-chat --reasoning-effort high

# 用上一次返回的 sessionId 续接，本次可以换模型
& $run --task "接着上一步继续" --session-id session-xxx --model deepseek-chat
```

子进程是全新会话，不继承父 Agent 的对话，任务文本必须自带上下文和目标。一次调用只跑一段任务；多步工作用 `--session-id` 串起来。

常用参数：

| 参数 | 默认 | 含义 |
|---|---|---|
| `--task <text>` | 位置参数，否则 stdin | 任务文本 |
| `--session-id <id>` | — | 续接已持久化的会话 |
| `--cwd <dir>` | 当前目录 | 子会话的工作区根 |
| `--provider <name>` | `deepseek-official` | provider 路由 |
| `--model <id>` | `deepseek-flash` | 模型 id。和 `--provider` 一起才能确定一个模型 |
| `--model-pool <项>` | — | 模型池的一项，可重复。字段用竖线分开，模型 id 可以含 `/` |
| `--reasoning-effort <e>` | 不填 | 只作用于 `--provider`/`--model`。空或 `default` 表示不写该字段，沿用模型默认等级 |
| `--patch <file>` | — | 额外的 `--patch` 文件，可重复 |
| `--retries <n>` | 1 | 失败后的总尝试次数。网络抖动建议 5 |
| `--retry-delay <sec>` | 2 | 重试间隔 |
| `--raw` | — | 只打印最终答案，不打印 JSON 摘要 |

覆盖模型时，调用器会写一份临时 patch。`agent-default-model` 的 `config` 是整体替换，所以 `provider` 和 `model` 会一起写上。思考等级留空时不写 `reasoningEffort`：dsh 只在这个字段缺失时使用该模型的 adapter 默认等级。字面量 `default` 同样按不填处理，不会写成 `reasoningEffort: default`。

一个模型是 `provider` + `model`。模型池的一项写成 `provider|model` 或 `provider|model|effort`，例如 `providerA|model1|high` 和 `ark|volcengine/deepseek-v4.1-flash`。竖线只分隔字段，模型 id 里的 `/` 会原样保留。在 cmd 和 PowerShell 里整项要加引号，因为 `|` 是管道符。调用器从第一项开始；某一项在 `--retries` 全部失败后才换下一项，并在 stderr 打印 `model pool switch`。池内换项要告诉用户，但这不是失败回退。整个池都失败后，才回退到 `deepseek-official`（先 `deepseek-flash`，已经试过则 `deepseek-chat`），再失败才改用外部 Agent 自己的 subagent。

## 返回

默认在 stdout 打印一行 JSON：

```json
{"exitCode":0,"sessionId":"session-xxx","answer":"...","reason":null,"error":null}
```

- `exitCode`：`0` 表示完成；`1` 表示中止、出错或没有轮次；参数或环境不合法时调用器自己返回 `2`。
- `sessionId`：下次 `--session-id` 用。
- `answer`：`final.text`，无损最终答案。
- `reason` / `error`：失败原因。诊断也出现在 stderr 的 `dsh: <code>: <message>` 行。

dsh 自己的 stdout 是 NDJSON 事件流（`session`、`status`、`text`、`thinking`、`tool_call`、`tool_result`、`final`）。调用器解析后只交出上面的摘要；推理和诊断留在 stderr。

## 版本

当前版本是 **v1.0.1**。版本号只写在仓库根目录的 `VERSION` 里。`scripts\build.cmd` 读取它，生成 `scripts\version.h`，再编进 `dsh-subagent.exe`。

```bat
scripts\dsh-subagent.exe --version
```

输出 `dsh-subagent v1.0.1`。改版本时只改 `VERSION`，然后重新编译。

## 重新编译与测试

在仓库根目录执行 `tests\run-smoke.cmd`。它会调用 `scripts\build.cmd` 编译 `scripts\dsh-subagent.exe`，再用假的 `node.exe` 跑冒烟测试。

编译器是 MinGW-w64 的 UCRT gcc。脚本先找 PATH 上的 `gcc.exe`，找不到就用 `C:\msys64\ucrt64\bin\gcc.exe`，并把该目录加进 PATH，否则 `cc1.exe` 找不到 MSYS2 自己的库。编译前脚本会把 `VERSION` 写成 `scripts\version.h`。等价的链接参数：

```bat
gcc -municode -O2 -s -Wall -Wextra -static-libgcc -o dsh-subagent.exe dsh-subagent.c
```

`-static-libgcc` 把 libgcc 收进 exe。不要加 `-static`，那会改成静态链接 CRT。成品只应导入 `KERNEL32.dll` 和 `api-ms-win-crt-*.dll`。

## 换行

文本文件在仓库里统一为 LF，由 `.gitattributes` 的 `* text=auto eol=lf` 固定。本仓库本地还设置了 `core.autocrlf=false` 与 `core.eol=lf`。`*.cmd` / `*.bat` 例外，必须是 CRLF，否则 `cmd.exe` 会把括号块解析坏。`*.exe` 按二进制保存，不参与换行转换。
