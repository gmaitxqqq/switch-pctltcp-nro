// pctltcp-nro — Switch Parental Control TCP Server (NRO)
// =============================================================
// Pure .nro homebrew app with:
//   - Console UI for on-device management
//   - Background TCP server (port 6000) in a pthread
//   - pctl IPC for parental control play timer operations
//
// Based on:
//   - switch-parental-timer v11.5 (pctl IPC, UI patterns)
//   - switch-play-timer-tcp v2.0.1 (TCP server, protocol)
//
// Key points:
//   - pctlInitialize() works in .nro process (not sysmodule)
//   - TCP server runs in a pthread, doesn't block UI
//   - Displays Switch IP for PC client connection
//   - PlayTimerSettings layout: u16[34], per-day at [7+4n], minutes at [7+4n+2]
//
// CRITICAL: printf requires consoleUpdate(NULL) to flush to screen!
//
// Compatible: Atmosphere CFW + fw 22.1.0 (AMS 1.11.1)
// =============================================================

#include <switch.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <arpa/inet.h>

#include "tcp_server.h"
#include "pctl_handler.h"

// ---- Constants ----
#define PT_DAY_NOLIMIT 0xFFFFu

// ---- Pad Input (new libnx API) ----
static PadState g_pad;

static void initPad(void)
{
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&g_pad);
}

static u64 padGetDown(void)
{
    padUpdate(&g_pad);
    return padGetButtonsDown(&g_pad);
}

// ---- UI Helpers ----
static const char *day_names[7] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};

static void printSeparator(void)
{
    printf("  ========================================\n");
}

static void consoleFlush(void)
{
    consoleUpdate(NULL);
}

static void waitForKey(void)
{
    printf("\n   Press any key to continue...\n");
    consoleFlush();
    while (appletMainLoop()) {
        u64 k = padGetDown();
        if (k) break;
        consoleFlush();
        svcSleepThread(10000000ULL);
    }
}

// ---- Get Switch IP Address ----
// nifm must be initialized before calling this
static void getIpAddressStr(char *buf, size_t buf_size)
{
    buf[0] = '\0';

    u32 ip = 0;
    Result rc = nifmGetCurrentIpAddress(&ip);

    if (R_FAILED(rc)) {
        snprintf(buf, buf_size, "N/A");
        return;
    }

    struct in_addr addr;
    addr.s_addr = ip;
    snprintf(buf, buf_size, "%s", inet_ntoa(addr));
}

// ---- Menu Screens ----

static void showStatus(void)
{
    consoleClear();
    printf("\n");
    printSeparator();
    printf("   Current Status\n");
    printSeparator();
    printf("\n");
    consoleFlush();

    // TCP server info
    char ip_str[64];
    getIpAddressStr(ip_str, sizeof(ip_str));
    printf("   TCP Server: %s:%d\n", ip_str, TCP_PORT);
    printf("   Clients:    %u connected\n\n", tcp_server_client_count());
    consoleFlush();

    // pctl play timer status
    bool enabled = false, restricted = false;
    u64 remaining_ns = 0;

    if (R_SUCCEEDED(pctl_is_enabled(&enabled)))
        printf("   Timer:       %s\n", enabled ? "Running" : "Stopped");
    else
        printf("   Timer:       (query failed)\n");

    if (R_SUCCEEDED(pctl_is_restricted(&restricted)))
        printf("   Restricted:  %s\n", restricted ? "YES (time up)" : "No");
    else
        printf("   Restricted:  (query failed)\n");

    if (R_SUCCEEDED(pctl_get_remaining_time(&remaining_ns)) && remaining_ns > 0) {
        u64 rem_min = remaining_ns / 60000000000ULL;
        printf("   Remaining:   %llu min\n", (unsigned long long)rem_min);
    }

    printf("\n");
    consoleFlush();

    // Daily limits
    PlayTimerSettings settings;
    printf("   Daily Time Limits (minutes):\n");
    if (R_SUCCEEDED(pctl_get_settings(&settings))) {
        for (int i = 0; i < 7; i++) {
            u16 m = settings.raw[PCTL_DAY_MINUTES_OFFSET(i)];
            if (m == PT_DAY_NOLIMIT)
                printf("     %s: No limit\n", day_names[i]);
            else
                printf("     %s: %u min (%uh %um)\n", day_names[i],
                       m, m / 60, m % 60);
        }
    } else {
        printf("     (Could not read timer settings)\n");
    }

    waitForKey();
}

static void menuSetWeeklyPlayTime(void)
{
    u16 days[7];

    // Read current values
    PlayTimerSettings settings;
    if (R_SUCCEEDED(pctl_get_settings(&settings))) {
        for (int i = 0; i < 7; i++)
            days[i] = settings.raw[PCTL_DAY_MINUTES_OFFSET(i)];
    } else {
        for (int i = 0; i < 7; i++) days[i] = PT_DAY_NOLIMIT;
    }

    int cursor = 0;  // 0..6 = per-day, 7 = apply, 8 = cancel
    bool editing_value = false;
    u16 edit_val = 0;
    bool done = false;

    while (appletMainLoop() && !done) {
        u64 k = padGetDown();

        consoleClear();
        printf("\n");
        printSeparator();
        printf("   Set Weekly Play Time\n");
        printSeparator();
        printf("\n");
        printf("   Daily play time limits (minutes)\n");
        printf("   0=blocked all day  65535=no limit\n\n");

        for (int i = 0; i < 7; i++) {
            bool sel = (!editing_value && cursor == i);
            if (editing_value && cursor == i) {
                printf("   %s %s [%u min]  <- editing\n",
                       sel ? ">" : " ",
                       day_names[i], edit_val);
                printf("     Up/Down: +/-1  L/R: +/-10\n");
                printf("     A=Confirm  B=Cancel\n");
            } else {
                if (days[i] == PT_DAY_NOLIMIT)
                    printf("   %s %s  No limit\n",
                           sel ? ">" : " ", day_names[i]);
                else
                    printf("   %s %s  %u min (%uh %um)\n",
                           sel ? ">" : " ", day_names[i],
                           days[i], days[i] / 60, days[i] % 60);
            }
        }

        printf("\n");
        printf("   %s [ Apply ]\n", (!editing_value && cursor == 7) ? ">" : " ");
        printf("   %s [ Cancel ]\n", (!editing_value && cursor == 8) ? ">" : " ");
        printf("\n");
        printf("   A=Select/Edit  B=Back  X=Set all 15min\n");
        consoleFlush();

        if (editing_value) {
            if (k & HidNpadButton_Up)    { if (edit_val < 65535) edit_val += 1; }
            if (k & HidNpadButton_Down)  { if (edit_val > 0) edit_val -= 1; }
            if (k & HidNpadButton_Right) { if (edit_val <= 65525) edit_val += 10; }
            if (k & HidNpadButton_Left)  { if (edit_val >= 10) edit_val -= 10; else edit_val = 0; }
            if (k & HidNpadButton_R)     { edit_val = PT_DAY_NOLIMIT; }
            if (k & HidNpadButton_L)     { edit_val = 0; }
            if (k & HidNpadButton_A)     { days[cursor] = edit_val; editing_value = false; }
            if (k & HidNpadButton_B)     { editing_value = false; }
        } else {
            if (k & HidNpadButton_Up)   { if (cursor > 0) cursor--; }
            if (k & HidNpadButton_Down) { if (cursor < 8) cursor++; }
            if (k & HidNpadButton_A) {
                if (cursor <= 6) {
                    editing_value = true;
                    edit_val = days[cursor];
                } else if (cursor == 7) {
                    // Apply — set each day individually for per-day control
                    Result last_rc = 0;
                    for (int d = 0; d < 7; d++) {
                        u32 mins = (days[d] == PT_DAY_NOLIMIT) ? 0 : days[d];
                        last_rc = pctl_set_day_limit_minutes(d, mins);
                    }
                    consoleClear();
                    printf("\n");
                    if (R_SUCCEEDED(last_rc))
                        printf("   Play time set successfully!\n");
                    else
                        printf("   Set failed: 0x%08X\n", (unsigned)last_rc);
                    consoleFlush();
                    svcSleepThread(1000000000ULL);
                    done = true;
                } else {
                    done = true;
                }
            }
            if (k & HidNpadButton_B) done = true;
            if (k & HidNpadButton_X) {
                for (int i = 0; i < 7; i++) days[i] = 15;
            }
        }

        svcSleepThread(50000000ULL);
    }
}

static void menuSetUniformTimer(void)
{
    u16 minutes = 15;
    bool done = false;

    while (appletMainLoop() && !done) {
        u64 k = padGetDown();

        consoleClear();
        printf("\n");
        printSeparator();
        printf("   Set Uniform Daily Time\n");
        printSeparator();
        printf("\n");
        printf("   Set the same play time limit for\n");
        printf("   every day of the week.\n\n");
        if (minutes == PT_DAY_NOLIMIT)
            printf("   Play time: [ No limit ]\n\n");
        else if (minutes == 0)
            printf("   Play time: [ 0 min (blocked) ]\n\n");
        else
            printf("   Play time: [ %u min ] (%uh %um)\n\n",
                   minutes, minutes / 60, minutes % 60);
        printf("   Up/Down:  +/- 1 min\n");
        printf("   Left/Right: +/- 10 min\n");
        printf("   L: Blocked (0 min)\n");
        printf("   R: No limit\n\n");
        printf("   A : Apply\n");
        printf("   B : Cancel\n");
        consoleFlush();

        if (k & HidNpadButton_Up)    { if (minutes < 65535) minutes += 1; if (minutes == PT_DAY_NOLIMIT) minutes = 65534; }
        if (k & HidNpadButton_Down)  { if (minutes > 0) minutes -= 1; }
        if (k & HidNpadButton_Right) { if (minutes <= 65525) minutes += 10; }
        if (k & HidNpadButton_Left)  { if (minutes >= 10) minutes -= 10; else minutes = 0; }
        if (k & HidNpadButton_L)     { minutes = 0; }
        if (k & HidNpadButton_R)     { minutes = PT_DAY_NOLIMIT; }

        if (k & HidNpadButton_A) {
            u32 mins = (minutes == PT_DAY_NOLIMIT) ? 0 : minutes;
            Result rc = pctl_set_daily_limit_minutes(mins);
            consoleClear();
            printf("\n");
            if (R_SUCCEEDED(rc)) {
                if (minutes == PT_DAY_NOLIMIT)
                    printf("   Time limit cleared (no limit)!\n");
                else if (minutes == 0)
                    printf("   Blocked all day!\n");
                else
                    printf("   Set %u min/day successfully!\n", minutes);
            } else {
                printf("   Failed: 0x%08X\n", (unsigned)rc);
            }
            waitForKey();
            done = true;
        }
        if (k & HidNpadButton_B) done = true;

        svcSleepThread(50000000ULL);
    }
}

static void menuClearPlayTimer(void)
{
    consoleClear();
    printf("\n");
    printSeparator();
    printf("   Clear Play Time Limits\n");
    printSeparator();
    printf("\n");
    printf("   Remove daily play time limits.\n");
    printf("   The timer will be disabled.\n\n");
    printf("   Press A to confirm, B to cancel.\n");
    consoleFlush();

    while (appletMainLoop()) {
        u64 k = padGetDown();
        if (k & HidNpadButton_A) {
            Result rc = pctl_set_daily_limit_minutes(0);
            consoleClear();
            printf("\n");
            if (R_SUCCEEDED(rc))
                printf("   Play time limits cleared!\n");
            else
                printf("   Clear failed: 0x%08X\n", (unsigned)rc);
            waitForKey();
            break;
        }
        if (k & HidNpadButton_B) break;
        consoleFlush();
        svcSleepThread(10000000ULL);
    }
}

static void menuUnlockTemporarily(void)
{
    consoleClear();
    printf("\n");
    printSeparator();
    printf("   Unlock Temporarily\n");
    printSeparator();
    printf("\n");
    printf("   Auto-reads current PIN and temporarily\n");
    printf("   lifts parental control restrictions.\n\n");
    printf("   Press A to confirm, B to cancel.\n");
    consoleFlush();

    while (appletMainLoop()) {
        u64 k = padGetDown();
        if (k & HidNpadButton_A) {
            // Use pctl IPC directly: read PIN via cmd 1208, then unlock via cmd 1201
            pctl_init();  // reinit to get fresh session
            Service *srv = pctlGetServiceSession_Service();
            if (!srv) {
                consoleClear();
                printf("\n   pctl service unavailable!\n");
                waitForKey();
                break;
            }

            // Read PIN
            char pin[32];
            memset(pin, 0, sizeof(pin));
            u32 pin_len = 0;
            Result rc = serviceDispatchOut(srv, 1208, pin_len,
                .buffer_attrs = { SfBufferAttr_HipcPointer | SfBufferAttr_Out },
                .buffers      = { { pin, sizeof(pin) } });

            if (R_FAILED(rc)) {
                consoleClear();
                printf("\n   Failed to read PIN: 0x%08X\n", (unsigned)rc);
                waitForKey();
                break;
            }

            // Unlock
            size_t n = (pin_len > 0 && pin_len < (u32)sizeof(pin)) ? ((size_t)pin_len + 1) : sizeof(pin);
            rc = serviceDispatch(srv, 1201,
                .buffer_attrs = { SfBufferAttr_HipcPointer | SfBufferAttr_In },
                .buffers      = { { pin, n } });

            consoleClear();
            printf("\n");
            if (R_SUCCEEDED(rc))
                printf("   Unlocked successfully!\n");
            else
                printf("   Unlock failed: 0x%08X\n", (unsigned)rc);
            waitForKey();
            break;
        }
        if (k & HidNpadButton_B) break;
        consoleFlush();
        svcSleepThread(10000000ULL);
    }
}

// ---- Main Menu ----

int main(int argc, char **argv)
{
    (void)argc; (void)argv;

    consoleInit(NULL);

    // Show splash immediately
    consoleClear();
    printf("\n");
    printSeparator();
    printf("   Switch Parental Control\n");
    printf("   TCP Server - NRO Edition\n");
    printf("   %s\n", VERSION_S);
    printSeparator();
    printf("\n");
    printf("   Initializing...\n");
    consoleFlush();

    // Initialize pad
    initPad();

    // Initialize pctl service
    Result pctl_rc = pctl_init();
    if (R_FAILED(pctl_rc)) {
        printf("\n   !! pctl init failed: 0x%08X\n", (unsigned)pctl_rc);
        printf("   !! Make sure CFW is active.\n\n");
        consoleFlush();
    } else {
        printf("   pctl service: OK\n");
        consoleFlush();
    }

    // Initialize socket driver
    Result sock_rc = socketInitializeDefault();
    if (R_FAILED(sock_rc)) {
        printf("   !! socket init failed: 0x%08X\n", (unsigned)sock_rc);
        waitForKey();
        consoleExit(NULL);
        return 1;
    }
    printf("   Socket driver: OK\n");
    consoleFlush();

    // Initialize nifm for IP address display
    Result nifm_rc = nifmInitialize(NifmServiceType_User);
    printf("   Network: %s\n", R_SUCCEEDED(nifm_rc) ? "OK" : "N/A");
    consoleFlush();

    // Start TCP server
    Result tcp_rc = tcp_server_start();
    if (R_FAILED(tcp_rc)) {
        printf("\n   !! TCP server failed: 0x%08X\n", (unsigned)tcp_rc);
        waitForKey();
        if (R_SUCCEEDED(nifm_rc)) nifmExit();
        if (R_SUCCEEDED(pctl_rc)) pctl_exit();
        socketExit();
        consoleExit(NULL);
        return 1;
    }
    printf("   TCP server:   OK (port %d)\n", TCP_PORT);
    consoleFlush();

    // Get and display IP
    char ip_str[64];
    getIpAddressStr(ip_str, sizeof(ip_str));
    printf("   IP Address:   %s\n", ip_str);
    consoleFlush();

    // Ready
    printf("\n");
    printSeparator();
    printf("   READY - Waiting for connections\n");
    printSeparator();
    printf("\n");
    consoleFlush();
    svcSleepThread(1500000000ULL);  // 1.5 sec splash

    // Main menu
    int cursor = 0;
    const int menu_count = 6;
    const char *menu_items[] = {
        "View Status",
        "Set Weekly Play Time (per day)",
        "Set Uniform Daily Time",
        "Clear Play Time Limits",
        "Unlock Temporarily",
        "Exit",
    };

    while (appletMainLoop()) {
        u64 k = padGetDown();

        consoleClear();
        printf("\n");
        printSeparator();
        printf("   Switch Parental Control TCP\n");
        printf("   %s | %s:%d | Clients: %u\n",
               VERSION_S, ip_str, TCP_PORT, tcp_server_client_count());
        printSeparator();
        printf("\n");

        if (R_FAILED(pctl_rc)) {
            printf("   !! pctl init failed: 0x%08X\n", (unsigned)pctl_rc);
            printf("   !! CFW required for pctl features\n\n");
        }

        for (int i = 0; i < menu_count; i++) {
            printf("   %s %s\n", (cursor == i) ? ">" : " ", menu_items[i]);
        }

        printf("\n");
        printf("   Up/Down: Navigate   A: Select   B: Exit\n");
        consoleFlush();

        if (k & HidNpadButton_Up)   { if (cursor > 0) cursor--; }
        if (k & HidNpadButton_Down) { if (cursor < menu_count - 1) cursor++; }

        if (k & HidNpadButton_B) break;

        if (k & HidNpadButton_A) {
            if (R_FAILED(pctl_rc) && cursor != 0) {
                // Only status and exit work without pctl
                continue;
            }

            switch (cursor) {
                case 0: showStatus(); break;
                case 1: menuSetWeeklyPlayTime(); break;
                case 2: menuSetUniformTimer(); break;
                case 3: menuClearPlayTimer(); break;
                case 4: menuUnlockTemporarily(); break;
                case 5: goto done;
            }
        }

        svcSleepThread(50000000ULL);
    }

done:
    // Cleanup
    printf("\n   Shutting down...\n");
    consoleFlush();

    tcp_server_stop();

    if (R_SUCCEEDED(nifm_rc)) nifmExit();
    if (R_SUCCEEDED(pctl_rc)) pctl_exit();
    socketExit();
    consoleExit(NULL);
    return 0;
}
