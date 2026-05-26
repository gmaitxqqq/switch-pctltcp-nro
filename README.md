# pctltcp-nro

Switch Parental Control TCP Server - NRO Edition

A pure .nro homebrew app for Nintendo Switch that provides:
- **Console UI** for on-device parental control management
- **TCP server** (port 6000) for remote management via PC client
- **pctl IPC** for play timer settings, unlock, and status queries

## Features

### On-Switch (Console UI)
- View current parental control status
- Set weekly play time (per-day or uniform)
- Clear play time limits
- Unlock restrictions temporarily
- Displays IP address for PC client connection

### PC Client (Python/Tkinter)
- Connect to Switch via TCP (port 6000)
- Set per-day or uniform daily time limits
- Start/Stop/Reset play timer
- Real-time status monitoring (auto-refresh every 10s)
- Connection status indicator

## Requirements

- **Switch**: Atmosphere CFW, firmware 22.1.0+ (AMS 1.11.1)
- **PC**: Python 3.7+ (standard library only, no pip needed)

## Usage

### Switch
1. Copy `pctltcp-nro.nro` to `/switch/` on your SD card
2. Launch from Homebrew Menu (Album)
3. Note the IP address displayed on screen
4. Use the console UI to manage settings directly, or connect via PC

### PC Client
1. Make sure your PC and Switch are on the same network
2. Run: `python client/swpc_client.py`
3. Enter the Switch IP address shown on screen
4. Click "Connect" to start managing

## Build

Requires [devkitPro](https://devkitpro.org/) with `libnx`:

```bash
make -j$(nproc)
```

Output: `pctltcp-nro.nro`

## Architecture

```
source/
  main.c           - Console UI + TCP thread management
  tcp_server.c/h   - TCP command server (pthread-based)
  pctl_handler.c/h - pctl IPC wrapper (Switch parental control)
client/
  swpc_client.py   - PC GUI client (Tkinter)
```

### TCP Protocol
Text-based, newline-delimited:
- `PING` → `PONG <ver>`
- `VERSION` → `pctltcp-nro <ver>`
- `STATUS` → `STATUS <enabled|disabled> <remaining_min> <daily_limit> <restricted|free>`
- `GET` → `PLAYTIME <minutes>`
- `SET <minutes>` → `OK PLAYTIME <minutes>` (all 7 days)
- `SET_DAY <day> <minutes>` → `OK DAY <day> <minutes>`
- `START` → `OK STARTED`
- `STOP` → `OK STOPPED`
- `RESET` → `OK RESET`
- `REMAINING` → `REMAINING <minutes>`

## Related Projects

- [switch-play-timer-tcp](https://github.com/gmaitxqqq/switch-play-timer-tcp) - Sysmodule (background) version
- [switch-parental-timer](https://github.com/gmaitxqqq/switch-parental-timer) - Standalone console-only version
