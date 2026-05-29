# Switch 家长控制 TCP 远程管理

通过 TCP 连接远程管理 Switch 的游戏时间限制。Switch 上运行 `.nro` 启动 TCP 服务端（端口 6000），PC 端用 Python 客户端远程设置每日限额、启停计时、查看状态。

**版本**：v1.5.0 | **固件**：兼容 Atmosphere 22.1.0+

---

## 适用场景

> 固定 IP 的局域网环境，PC 常驻在同一网络中，通过 TCP 客户端精细化管理。

---

## 安装

1. 从 [Releases](../../releases) 下载最新版 zip
2. 解压得到 `pctltcp-nro.nro` 和 `swpc_client.py`
3. `.nro` 复制到 SD 卡 `/switch/` 目录
4. PC 客户端需要 Python 3.7+（仅标准库，无需 pip 安装）

---

## 使用方法

### Switch 端

1. Homebrew Menu 启动 `pctltcp-nro`
2. 屏幕显示 IP 地址（记下来）
3. 可直接在 Switch 上操作控制台菜单，也可用 PC 客户端远程管理

### PC 客户端

1. 确保 PC 和 Switch 在同一局域网
2. 运行：`python swpc_client.py`
3. 输入 Switch 屏幕上显示的 IP，点击「连接」
4. 即可管理游戏时间

### PC 客户端功能

- 设置当天 / 指定星期 / 全部 7 天的每日限额
- 启动 / 暂停计时、重置今日已用时间
- 实时状态监控（10 秒自动刷新）
- 显示计时状态、今日限额、剩余时间、是否限制

---

## TCP 协议

文本协议，换行符 `\n` 分隔，适合自定义客户端集成：

| 命令 | 响应 | 说明 |
|------|------|------|
| `PING` | `PONG <ver>` | 心跳 |
| `STATUS` | `STATUS <on|off> <剩余分> <限额分> <restricted|free>` | 查询状态 |
| `SET <分钟>` | `OK PLAYTIME <分钟>` | 设置全部 7 天统一限额（0=不限） |
| `SET_DAY <日> <分钟>` | `OK DAY <日> <分钟>` | 设置某天限额（日: 0=周日..6=周六） |
| `START` | `OK STARTED` | 启动计时 |
| `STOP` | `OK STOPPED` | 暂停计时 |
| `RESET` | `OK RESET` | 重置今日已用时间 |

连接后服务端发送：`HELLO pctltcp-nro <ver>`

---

## 项目结构

```
switch-pctltcp-nro/
├── source/
│   ├── main.c              # 控制台 UI + 主循环
│   ├── tcp_server.c/h      # TCP 命令服务端（pthread）
│   └── pctl_handler.c/h    # pctl IPC 封装
├── client/
│   └── swpc_client.py      # PC 图形客户端（Tkinter）
├── pctltcp-nro.icon        # NRO 图标
├── pctltcp-nro.jpg         # NRO 图标源图
├── config.json             # NRO 元数据
└── Makefile
```

---

## 从源码编译

```bash
export DEVKITPRO=/opt/devkitpro
make -j$(nproc)
```

推送至 GitHub 后 Actions 自动构建。

---

## 同系列工具

| 项目 | 类型 | 适用场景 |
|------|------|---------|
| [switch-parental-timer](https://github.com/gmaitxqqq/switch-parental-timer) | 本机 NRO | 在 Switch 上直接操作，无需网络 |
| **switch-pctltcp-nro**（本仓库） | 前台 NRO + TCP | 固定 IP 局域网，PC 客户端远程管理 |
| [switch-pctltcp-web](https://github.com/gmaitxqqq/switch-pctltcp-web) | 前台 NRO + Web UI | 外出时手机浏览器管理（无固定 IP） |
| [switch-pctltcp-sysmodule](https://github.com/gmaitxqqq/switch-pctltcp-sysmodule) | 后台 sysmodule | 固定 IP 家庭环境，开机自动运行 |

---

## 版本历史

| 版本 | 变更 |
|------|------|
| **v1.5.0** | 修复 STATUS 命令每日限额读取错误；客户端 Treeview 显示修复；减少 pctl session 频繁重置 |
| **v1.4.0** | 修复 pctl_set_settings IPC 调用错误；STATUS/START/STOP 稳定性改进 |
| **v1.2.0** | 基本功能完成：SET/SET_DAY/START/STOP/RESET/STATUS；PC 客户端初始版本 |
