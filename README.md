# pctltcp-nro

Nintendo Switch 家长控制 TCP 远程管理工具（NRO 版）

通过 TCP 连接远程管理 Switch 的游戏时间限制。在 Switch 上运行 `.nro` 后，PC 客户端可远程设置每日限额、启动/暂停计时、查看实时状态。

## 功能

### Switch 端（pctltcp-nro.nro）
- 控制台 UI 直接管理家长控制设置
- TCP 服务端（端口 6000）供 PC 远程连接
- 支持查看/设置每周游戏时间（按天或统一）
- 启动/暂停/重置计时器
- 临时解锁家长控制
- 启动后显示 IP 地址方便连接

### PC 客户端（swpc_client.py）
- 图形界面，Tkinter 实现，无需 pip 安装
- 设置当天/指定星期/全部 7 天的每日限额
- 启动/暂停计时、重置今日已用时间
- 实时状态监控（10 秒自动刷新）
- 计时状态、今日限额、剩余时间、是否限制

## 环境要求

| 组件 | 要求 |
|------|------|
| Switch | Atmosphere CFW, 固件 22.1.0+, AMS 1.11.1+ |
| PC | Python 3.7+（仅标准库） |

## 使用方法

### Switch 端

1. 下载 `pctltcp-nro.nro` 放到 SD 卡 `/switch/` 目录
2. Homebrew Menu（相册）启动
3. 屏幕会显示 IP 地址，记下来
4. 可直接在 Switch 上用控制台操作，也可用 PC 客户端远程管理

### PC 客户端

1. 确保 PC 和 Switch 在同一局域网
2. 运行：`python swpc_client.py`
3. 输入 Switch 屏幕上显示的 IP，点击"连接"
4. 开始管理游戏时间

## TCP 协议

文本协议，换行符分隔（`\n`）：

| 命令 | 响应 | 说明 |
|------|------|------|
| `PING` | `PONG <ver>` | 心跳检测 |
| `STATUS` | `STATUS <on\|off> <剩余分> <限额分> <restricted\|free>` | 查询状态 |
| `SET <分钟>` | `OK PLAYTIME <分钟>` | 设置全部 7 天统一限额（0=不限） |
| `SET_DAY <日> <分钟>` | `OK DAY <日> <分钟>` | 设置某天限额（日: 0=周日..6=周六） |
| `START` | `OK STARTED` | 启动计时 |
| `STOP` | `OK STOPPED` | 暂停计时 |
| `RESET` | `OK RESET` | 重置今日已用时间 |

连接后服务端会发送 HELLO 消息：`HELLO pctltcp-nro <ver>`

## 项目结构

```
pctltcp-nro/
├── source/
│   ├── main.c              # 控制台 UI + 主循环
│   ├── tcp_server.c/h      # TCP 命令服务端（pthread）
│   └── pctl_handler.c/h    # pctl IPC 封装（家长控制）
├── client/
│   └── swpc_client.py      # PC 图形客户端（Tkinter）
├── .github/workflows/
│   └── build.yml           # CI 自动编译
└── Makefile
```

## 编译

需要 [devkitPro](https://devkitpro.org/) 环境：

```bash
make -j$(nproc)
```

输出：`pctltcp-nro.nro`

CI 使用 GitHub Actions + `devkitpro/devkita64` Docker 镜像自动编译。

## 版本历史

### v1.4.0 (nro-1.4) — 2026-05-27
- **修复**：STATUS 命令读取今日限额始终显示固定值（`day=0` 硬编码为周日），改为动态读取当天对应的限额
- **修复**：客户端 Treeview 状态面板显示 `---`（`set()` 调用方式错误）
- **修复**：非写操作函数中多余的 `pctl_reinit()` 导致 session 频繁重置
- 客户端版本 v1.3.6

### v1.3.0 (nro-1.3) — 2026-05-27
- **修复**：`pctl_set_settings()` IPC 调用错误（多余 buffer 描述符导致 0xF601）
- STATUS/START/STOP 稳定性改进

### v1.2.0 (nro-1.2)
- 基本功能完成：SET/SET_DAY/START/STOP/RESET/STATUS
- PC 客户端初始版本

## 相关项目

- [switch-play-timer-tcp](https://github.com/gmaitxqqq/switch-play-timer-tcp) — Sysmodule（后台）版本，开机自启
- [switch-parental-timer](https://github.com/gmaitxqqq/switch-parental-timer) — 纯控制台独立版本

## 许可证

MIT
