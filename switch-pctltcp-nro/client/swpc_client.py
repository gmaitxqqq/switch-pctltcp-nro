#!/usr/bin/env python3
"""
SWPC 客户端 v1.3.4 - Nintendo Switch 游戏时间 TCP 远程管理 (NRO 版)
====================================================================
通过 TCP（端口 6000）连接到 Switch，远程管理家长控制的游戏时间限制。

运行要求：Python 3.7+（仅标准库，无需 pip 安装）
Switch 端：需运行 pctltcp-nro.nro（Homebrew Menu 启动）
"""

import socket
import threading
import time
import traceback
from dataclasses import dataclass
from datetime import datetime

import tkinter as tk
from tkinter import ttk, messagebox

VERSION = "1.3.4"

# ---------------------------------------------------------------------------
# 协议常量
# ---------------------------------------------------------------------------
DEFAULT_PORT = 6000
TIMEOUT_CONNECT = 8.0
TIMEOUT_COMMAND = 5.0

# Switch 星期映射: 0=周日, 1=周一, ..., 6=周六
SWITCH_DAY_NAMES = ["周日", "周一", "周二", "周三", "周四", "周五", "周六"]


def get_today_switch_day() -> int:
    """返回今天的 Switch 星期序号（0=周日..6=周六）。"""
    py_weekday = datetime.now().weekday()  # 0=周一..6=周日
    return (py_weekday + 1) % 7


def get_today_name() -> str:
    return SWITCH_DAY_NAMES[get_today_switch_day()]


@dataclass
class SwitchStatus:
    """Switch 游戏计时器状态快照"""
    connected: bool = False
    enabled: bool = False
    restricted: bool = False
    daily_limit_minutes: int = 0
    remaining_minutes: int = 0

    @staticmethod
    def from_response(line: str) -> "SwitchStatus":
        """解析 STATUS 响应行"""
        import re
        m = re.match(
            r"STATUS\s+(enabled|disabled)\s+(\d+)\s+(\d+)\s+(restricted|free)",
            line.strip(),
        )
        if m:
            return SwitchStatus(
                connected=True,
                enabled=(m.group(1) == "enabled"),
                restricted=(m.group(4) == "restricted"),
                daily_limit_minutes=int(m.group(3)),
                remaining_minutes=int(m.group(2)),
            )
        raise ValueError(f"无法解析 STATUS 响应: {line}")


# ---------------------------------------------------------------------------
# TCP 客户端
# ---------------------------------------------------------------------------
class SwitchTCPClient:
    """管理与 Switch 上 pctltcp-nro 的 TCP 连接"""

    def __init__(self):
        self._sock = None
        self._lock = threading.Lock()

    def connect(self, host: str, port: int = DEFAULT_PORT) -> str:
        """连接到 Switch。成功返回空字符串，失败返回错误描述"""
        self.disconnect()
        try:
            self._sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            self._sock.settimeout(TIMEOUT_CONNECT)
            self._sock.connect((host, port))
            self._sock.settimeout(TIMEOUT_COMMAND)

            # 尝试读取 HELLO（2秒超时），没有也无所谓
            try:
                self._sock.settimeout(2.0)
                hello = self._sock.recv(1024).decode("utf-8", errors="replace")
                print(f"[DEBUG] HELLO: {hello.strip()}")
            except socket.timeout:
                pass  # 没有 HELLO，继续

            self._sock.settimeout(TIMEOUT_COMMAND)

            # 发 PING，等 PONG
            self._sock.sendall(b"PING\n")
            reply = self._sock.recv(1024).decode("utf-8", errors="replace")
            print(f"[DEBUG] PING reply: {reply.strip()}")

            if "PONG" not in reply:
                self.disconnect()
                return f"协议错误: 收到 '{reply.strip()}'，期望 'PONG'"
            return ""
        except socket.timeout:
            self.disconnect()
            return f"连接超时({TIMEOUT_CONNECT}秒)。请检查 IP 是否正确，是否与 PC 在同一网络。"
        except ConnectionRefusedError:
            self.disconnect()
            return "连接被拒绝。pctltcp-nro 是否在 Switch 上运行？"
        except OSError as e:
            self.disconnect()
            return f"网络错误: {e}"

    def disconnect(self):
        """断开连接"""
        with self._lock:
            if self._sock:
                try:
                    self._sock.close()
                except OSError:
                    pass
                finally:
                    self._sock = None

    @property
    def is_connected(self) -> bool:
        return self._sock is not None

    def _send_cmd(self, cmd: str) -> str:
        """发送命令并返回响应行"""
        with self._lock:
            if not self._sock:
                raise ConnectionError("未连接")
            self._sock.settimeout(TIMEOUT_COMMAND)
            self._sock.sendall((cmd + "\n").encode("utf-8"))
            data = self._sock.recv(4096)
            return data.decode("utf-8", errors="replace")

    def get_status(self) -> SwitchStatus:
        """获取完整的游戏计时器状态"""
        reply = self._send_cmd("STATUS")
        print(f"[DEBUG] STATUS reply: {reply.strip()}")
        return SwitchStatus.from_response(reply)

    def set_limit(self, minutes: int) -> str:
        """设置全部 7 天统一的每日限额。返回响应字符串"""
        reply = self._send_cmd(f"SET {minutes}")
        print(f"[DEBUG] SET reply: {reply.strip()}")
        return reply

    def set_day_limit(self, day: int, minutes: int) -> str:
        """设置指定某天的限额。day: 0=周日..6=周六。返回响应字符串"""
        reply = self._send_cmd(f"SET_DAY {day} {minutes}")
        print(f"[DEBUG] SET_DAY reply: {reply.strip()}")
        return reply

    def start_timer(self) -> str:
        """启动游戏计时器（开始累计时间）"""
        return self._send_cmd("START")

    def stop_timer(self) -> str:
        """停止游戏计时器（暂停累计）"""
        return self._send_cmd("STOP")

    def reset_timer(self) -> str:
        """重置今日游戏时间（已用时间清零）"""
        return self._send_cmd("RESET")


# ---------------------------------------------------------------------------
# GUI 应用程序
# ---------------------------------------------------------------------------
class SWPCApp:
    """Switch 游戏时间控制的 Tkinter 图形界面"""

    def __init__(self, root: tk.Tk):
        self.root = root
        self.root.title(f"SWPC v{VERSION} - Switch 远程管理 (NRO)")
        self.root.geometry("560x650")
        self.root.resizable(True, True)

        self.client = SwitchTCPClient()
        self._polling = False
        self._poll_thread = None
        self._stop_event = threading.Event()

        self._build_ui()
        self._update_ui_state()

    # ---- 构建界面 ----

    def _build_ui(self):
        """构建所有 GUI 组件"""
        pad = {"padx": 12, "pady": 4}

        main = ttk.Frame(self.root, padding=10)
        main.pack(fill=tk.BOTH, expand=True)

        # ================================================================
        # 连接区域
        # ================================================================
        conn_frame = ttk.LabelFrame(main, text="连接", padding=8)
        conn_frame.pack(fill=tk.X, **pad)

        ttk.Label(conn_frame, text="Switch IP：").grid(row=0, column=0, sticky=tk.W)
        self.ip_var = tk.StringVar(value="192.168.31.143")
        ip_entry = ttk.Entry(conn_frame, textvariable=self.ip_var, width=18)
        ip_entry.grid(row=0, column=1, padx=5, sticky=tk.W)

        self.conn_btn = ttk.Button(conn_frame, text="连接", command=self._on_connect)
        self.conn_btn.grid(row=0, column=2, padx=5)

        self.status_led = tk.Canvas(conn_frame, width=16, height=16,
                                     highlightthickness=0)
        self.status_led.grid(row=0, column=3, padx=5)
        self._led_circle = self.status_led.create_oval(2, 2, 14, 14, fill="gray",
                                                        outline="gray")

        self.conn_label = ttk.Label(conn_frame, text="未连接", foreground="gray")
        self.conn_label.grid(row=1, column=0, columnspan=4, sticky=tk.W, **pad)

        # ================================================================
        # 时间设置区域（三行精细设置）
        # ================================================================
        set_frame = ttk.LabelFrame(main, text="时间设置", padding=8)
        set_frame.pack(fill=tk.X, **pad)

        # ---- 第1行：当天限额 ----
        row1 = ttk.Frame(set_frame)
        row1.pack(fill=tk.X, pady=2)

        today_name = get_today_name()
        ttk.Label(row1, text=f"当天（{today_name}）限额：").pack(side=tk.LEFT)

        self.today_limit_var = tk.IntVar(value=60)
        ttk.Spinbox(row1, from_=0, to=1440, increment=5,
                     textvariable=self.today_limit_var, width=6
                     ).pack(side=tk.LEFT, padx=5)

        ttk.Label(row1, text="分钟").pack(side=tk.LEFT)
        self.today_apply_btn = ttk.Button(row1, text="应用",
                                           command=self._on_set_today)
        self.today_apply_btn.pack(side=tk.LEFT, padx=8)

        ttk.Separator(set_frame, orient=tk.HORIZONTAL).pack(fill=tk.X, pady=4)

        # ---- 第2行：每日设置（下拉框选星期）----
        row2 = ttk.Frame(set_frame)
        row2.pack(fill=tk.X, pady=2)

        ttk.Label(row2, text="每日设置：").pack(side=tk.LEFT)

        self.day_var = tk.StringVar(value=SWITCH_DAY_NAMES[get_today_switch_day()])
        day_combo = ttk.Combobox(row2, textvariable=self.day_var,
                                  values=SWITCH_DAY_NAMES,
                                  state="readonly", width=6)
        day_combo.pack(side=tk.LEFT, padx=5)

        ttk.Label(row2, text="限额：").pack(side=tk.LEFT)

        self.day_limit_var = tk.IntVar(value=60)
        ttk.Spinbox(row2, from_=0, to=1440, increment=5,
                     textvariable=self.day_limit_var, width=6
                     ).pack(side=tk.LEFT, padx=5)

        ttk.Label(row2, text="分钟").pack(side=tk.LEFT)
        self.day_apply_btn = ttk.Button(row2, text="应用",
                                         command=self._on_set_day)
        self.day_apply_btn.pack(side=tk.LEFT, padx=8)

        ttk.Separator(set_frame, orient=tk.HORIZONTAL).pack(fill=tk.X, pady=4)

        # ---- 第3行：全部统一 ----
        row3 = ttk.Frame(set_frame)
        row3.pack(fill=tk.X, pady=2)

        ttk.Label(row3, text="全部统一：").pack(side=tk.LEFT)

        self.all_limit_var = tk.IntVar(value=60)
        ttk.Spinbox(row3, from_=0, to=1440, increment=5,
                     textvariable=self.all_limit_var, width=6
                     ).pack(side=tk.LEFT, padx=5)

        ttk.Label(row3, text="分钟（0=不限）").pack(side=tk.LEFT)
        self.all_apply_btn = ttk.Button(row3, text="应用到全部7天",
                                         command=self._on_set_all)
        self.all_apply_btn.pack(side=tk.LEFT, padx=8)

        # ================================================================
        # 控制区域
        # ================================================================
        ctrl_frame = ttk.LabelFrame(main, text="计时控制", padding=8)
        ctrl_frame.pack(fill=tk.X, **pad)

        hint = ttk.Label(ctrl_frame,
                          text="控制 Switch 端计时器的运行状态（非开关整个家长控制功能）",
                          foreground="gray")
        hint.pack(anchor=tk.W, pady=(0, 5))

        btn_frame = ttk.Frame(ctrl_frame)
        btn_frame.pack()

        self.start_btn = ttk.Button(btn_frame, text="▶ 启动计时",
                                     command=self._on_start)
        self.start_btn.pack(side=tk.LEFT, padx=3)
        ttk.Label(btn_frame, text="开始累计游戏时间", foreground="gray",
                   font=("", 8)).pack(side=tk.LEFT, padx=(0, 10))

        self.stop_btn = ttk.Button(btn_frame, text="⏸ 暂停计时",
                                    command=self._on_stop)
        self.stop_btn.pack(side=tk.LEFT, padx=3)
        ttk.Label(btn_frame, text="暂停累计（保留已用时间）", foreground="gray",
                   font=("", 8)).pack(side=tk.LEFT, padx=(0, 10))

        self.reset_btn = ttk.Button(btn_frame, text="↺ 重置今日",
                                     command=self._on_reset)
        self.reset_btn.pack(side=tk.LEFT, padx=3)
        ttk.Label(btn_frame, text="今日已用时间清零", foreground="gray",
                   font=("", 8)).pack(side=tk.LEFT)

        # ================================================================
        # 状态显示
        # ================================================================
        stat_frame = ttk.LabelFrame(main, text="Switch 状态", padding=8)
        stat_frame.pack(fill=tk.X, **pad)

        self.status_tree = ttk.Treeview(stat_frame, columns=("value",),
                                         show="tree headings", height=4)
        self.status_tree.heading("#0", text="项目")
        self.status_tree.heading("value", text="数值")
        self.status_tree.column("#0", width=140)
        self.status_tree.column("value", width=250)

        self._status_items = {}
        items = [
            ("timer_state", "计时状态"),
            ("daily_limit", "今日限额"),
            ("remaining",   "剩余时间"),
            ("restricted",  "是否限制"),
        ]
        for key, label in items:
            iid = self.status_tree.insert("", tk.END, text=label, values=("---",))
            self._status_items[key] = iid

        self.status_tree.pack(fill=tk.X)

        # 自动刷新
        refresh_frame = ttk.Frame(stat_frame)
        refresh_frame.pack(fill=tk.X, pady=(5, 0))
        self.auto_var = tk.BooleanVar(value=True)
        ttk.Checkbutton(refresh_frame, text="自动刷新（每10秒）",
                         variable=self.auto_var, command=self._on_auto_toggle).pack(side="left")

        self.refresh_btn = ttk.Button(refresh_frame, text="立即刷新",
                                       command=self._on_refresh)
        self.refresh_btn.pack(side="right")

        # ================================================================
        # 日志区域
        # ================================================================
        log_frame = ttk.LabelFrame(main, text="日志", padding=8)
        log_frame.pack(fill=tk.BOTH, expand=True, **pad)

        self.log_text = tk.Text(log_frame, height=6, wrap=tk.WORD, state=tk.DISABLED,
                                 font=("Microsoft YaHei", 9))
        log_scroll = ttk.Scrollbar(log_frame, orient=tk.VERTICAL,
                                    command=self.log_text.yview)
        self.log_text.configure(yscrollcommand=log_scroll.set)

        self.log_text.pack(side=tk.LEFT, fill=tk.BOTH, expand=True)
        log_scroll.pack(side=tk.RIGHT, fill=tk.Y)

    # ---- 事件处理 ----

    def _on_connect(self):
        """处理连接/断开按钮"""
        if self.client.is_connected:
            self.client.disconnect()
            self._stop_polling()
            self._update_ui_state()
            self._log("已断开连接。")
            return

        ip = self.ip_var.get().strip()
        self._log(f"正在连接 {ip}:{DEFAULT_PORT}...")
        self.conn_btn.configure(state=tk.DISABLED, text="连接中...")
        self.root.update_idletasks()

        # 用 after 延迟执行 connect，让 UI 先刷新
        self.root.after(100, lambda: self._do_connect(ip))

    def _do_connect(self, ip):
        """真正执行 connect（在 after 回调里）"""
        try:
            err = self.client.connect(ip)
        except Exception as e:
            err = f"异常: {type(e).__name__}: {e}"
            try:
                with open(r"C:\Users\HaiXin_LK7\Desktop\connect_detail.log", "a", encoding="utf-8") as f:
                    f.write(time.strftime("%H:%M:%S ") + err + "\n")
                    f.write(traceback.format_exc() + "\n")
            except Exception:
                pass

        if err:
            self._log(f"错误: {err}")
            self.client.disconnect()
            self.conn_btn.configure(text="连接", state=tk.NORMAL)
            self.conn_label.configure(text="未连接", foreground="gray")
        else:
            self._log(f"已连接到 {ip}！")
            self._update_ui_state()
            self._start_polling()
            self._on_refresh()

    def _update_ui_state(self):
        """根据连接状态更新组件状态"""
        connected = self.client.is_connected
        if connected:
            self.conn_btn.configure(text="断开", state=tk.NORMAL)
            self.status_led.itemconfig(self._led_circle, fill="#00cc00", outline="#00cc00")
            self.conn_label.configure(text="已连接", foreground="#006600")
            state = tk.NORMAL
        else:
            self.conn_btn.configure(text="连接", state=tk.NORMAL)
            self.status_led.itemconfig(self._led_circle, fill="gray", outline="gray")
            self.conn_label.configure(text="未连接", foreground="gray")
            state = tk.DISABLED

        self.today_apply_btn.configure(state=state)
        self.day_apply_btn.configure(state=state)
        self.all_apply_btn.configure(state=state)
        self.start_btn.configure(state=state)
        self.stop_btn.configure(state=state)
        self.reset_btn.configure(state=state)
        self.refresh_btn.configure(state=state)

    def _on_set_today(self):
        """设置当天限额"""
        if not self._check_connected():
            return
        day = get_today_switch_day()
        minutes = self.today_limit_var.get()
        day_name = SWITCH_DAY_NAMES[day]
        self._log(f"正在设置{day_name}限额为 {minutes} 分钟...")
        reply = self.client.set_day_limit(day, minutes)
        self._log(f"  -> {reply.strip()}")
        self._on_refresh()

    def _on_set_day(self):
        """设置指定星期几的限额"""
        if not self._check_connected():
            return
        day_name = self.day_var.get()
        try:
            day = SWITCH_DAY_NAMES.index(day_name)
        except ValueError:
            self._log("错误: 无效的星期选择")
            return
        minutes = self.day_limit_var.get()
        self._log(f"正在设置{day_name}限额为 {minutes} 分钟...")
        reply = self.client.set_day_limit(day, minutes)
        self._log(f"  -> {reply.strip()}")
        self._on_refresh()

    def _on_set_all(self):
        """设置全部 7 天统一限额"""
        if not self._check_connected():
            return
        minutes = self.all_limit_var.get()
        desc = "不限" if minutes == 0 else f"{minutes} 分钟"
        self._log(f"正在设置全部7天限额为 {desc}...")
        reply = self.client.set_limit(minutes)
        self._log(f"  -> {reply.strip()}")
        self._on_refresh()

    def _on_start(self):
        """启动游戏计时器"""
        if not self._check_connected():
            return
        reply = self.client.start_timer()
        self._log(f"启动计时: {reply.strip()}")
        self._on_refresh()

    def _on_stop(self):
        """暂停游戏计时器"""
        if not self._check_connected():
            return
        reply = self.client.stop_timer()
        self._log(f"暂停计时: {reply.strip()}")
        self._on_refresh()

    def _on_reset(self):
        """重置今日游戏时间"""
        if not self._check_connected():
            return
        if not messagebox.askyesno("确认重置",
                                    "重置今日游戏时间计数？\n"
                                    "孩子将重新获得完整的今日限额。"):
            return
        reply = self.client.reset_timer()
        self._log(f"重置今日: {reply.strip()}")
        self._on_refresh()

    def _on_refresh(self):
        """手动刷新 Switch 状态"""
        if not self.client.is_connected:
            return
        try:
            status = self.client.get_status()
            self._display_status(status)
        except Exception as e:
            self._log(f"刷新错误: {e}")

    def _on_auto_toggle(self):
        if self.auto_var.get():
            self._start_polling()
        else:
            self._stop_polling()

    # ---- 轮询 ----

    def _start_polling(self):
        if self._polling:
            return
        self._polling = True
        self._stop_event.clear()
        self._poll_thread = threading.Thread(target=self._poll_loop, daemon=True)
        self._poll_thread.start()

    def _stop_polling(self):
        self._polling = False
        self._stop_event.set()

    def _poll_loop(self):
        while not self._stop_event.is_set():
            if self.client.is_connected:
                try:
                    status = self.client.get_status()
                    self.root.after(0, lambda s=status: self._display_status(s))
                except Exception:
                    self.root.after(0, self._on_connection_lost)
                    break
            self._stop_event.wait(10.0)

    # ---- UI 状态 ----

    def _on_connection_lost(self):
        self._log("错误: 连接已断开！")
        self.client.disconnect()
        self._stop_polling()
        self._update_ui_state()
        self._clear_status()

    def _display_status(self, status: SwitchStatus):
        """用实时数据更新状态面板"""
        self.status_tree.set(
            self._status_items["timer_state"],
            value="运行中（正在计时）" if status.enabled else "已暂停",
        )
        self.status_tree.set(
            self._status_items["daily_limit"],
            value=f"{status.daily_limit_minutes} 分钟" if status.daily_limit_minutes > 0 else "不限",
        )
        self.status_tree.set(
            self._status_items["remaining"],
            value=f"{status.remaining_minutes} 分钟",
        )
        self.status_tree.set(
            self._status_items["restricted"],
            value="是（时间到，已锁屏）" if status.restricted else "否（可以继续玩）",
        )

    def _clear_status(self):
        """清空所有状态字段"""
        for iid in self._status_items.values():
            self.status_tree.set(iid, value="---")

    def _check_connected(self) -> bool:
        if not self.client.is_connected:
            self._log("错误: 未连接到 Switch。")
            return False
        return True

    def _log(self, msg: str):
        """追加带时间戳的日志信息"""
        ts = time.strftime("%H:%M:%S")
        self.log_text.configure(state=tk.NORMAL)
        self.log_text.insert(tk.END, f"[{ts}] {msg}\n")
        self.log_text.see(tk.END)
        self.log_text.configure(state=tk.DISABLED)

    # ---- 清理 ----

    def shutdown(self):
        """退出前清理资源"""
        self._stop_polling()
        self.client.disconnect()


# ---------------------------------------------------------------------------
# 入口
# ---------------------------------------------------------------------------
def main():
    root = tk.Tk()
    app = SWPCApp(root)

    def on_close():
        app.shutdown()
        root.destroy()

    root.protocol("WM_DELETE_WINDOW", on_close)
    root.mainloop()


if __name__ == "__main__":
    main()
