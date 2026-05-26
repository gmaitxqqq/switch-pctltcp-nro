/**
 * pctl_handler.c - Nintendo Switch PCTL service IPC wrapper
 *
 * Communicates with the pctl system service using libnx's
 * pctlInitialize() + serviceDispatch*() API.
 *
 * COMPATIBILITY NOTE (sysmodule vs .nro):
 *   libnx pctlInitialize() tries: pctl:a -> pctl:s -> pctl:r -> pctl
 *   - In .nro (user-mode): pctl:a succeeds, all commands work.
 *   - In sysmodule: pctl:a is likely denied; pctl:s should succeed.
 *     Read-only commands (GET, STATUS, REMAINING) should work on pctl:s.
 *     Write commands (SET, SET_DAY, RESET) use cmd 195101 which may
 *     require pctl:a. If it fails, the TCP client gets an ERR response.
 *
 * IPC command IDs (from SwitchBrew / NX-Pctl-Manager):
 *   1451: StartPlayTimer
 *   1452: StopPlayTimer
 *   1453: IsPlayTimerEnabled
 *   1454: GetPlayTimerRemainingTime
 *   1455: IsRestrictedByPlayTimer
 *   145601: GetPlayTimerSettings [18.0.0+]
 *   195101: SetPlayTimerSettingsForDebug [18.0.0+]
 *
 * PlayTimerSettings layout: u16[34] (0x44 bytes)
 *   [0]     header magic  (0x0101 when days are set)
 *   [1]     header flag   (0x0001 when enabled)
 *   [2-6]   reserved      (zero)
 *   Day n (Sun=0..Sat=6):
 *     [7+4n+0]  day flag    (0x0600 = configured)
 *     [7+4n+1]  day enable  (0x0100 = restricted, 0x0000 = skip)
 *     [7+4n+2]  day minutes (0=blocked, 1-1440=limit, 0xFFFF=unlimited)
 *     [7+4n+3]  day padding (zero)
 */

#include "pctl_handler.h"
#include <string.h>

/* ------------------------------------------------------------------ */
/* Global state                                                        */
/* ------------------------------------------------------------------ */
static Service s_pctlSrv;
static bool s_initialized = false;

/* ------------------------------------------------------------------ */
/* pctl_init: Use libnx pctlInitialize() for proper service setup      */
/* ------------------------------------------------------------------ */
Result pctl_init(void)
{
    Result rc;

    if (s_initialized)
        return 0;

    /* Use libnx's pctlInitialize() which handles:
     * - Opening pctl:a (or pctl:s / pctl:r fallbacks)
     * - Creating the IParentalControlService session
     * - Converting to domain
     * NOTE: In sysmodule context this may fail or even crash.
     * The caller must handle failure gracefully. */
    rc = pctlInitialize();
    if (R_FAILED(rc))
        return rc;

    /* Get the service session for direct IPC calls.
     * SAFETY CHECK: pctlGetServiceSession_Service() can return NULL
     * in sysmodule context if the internal state wasn't set up.
     * Accessing a NULL pointer causes Error 2345-0002 (NULL dereference). */
    Service *srv = pctlGetServiceSession_Service();
    if (srv == NULL) {
        pctlExit();
        return MAKERESULT(Module_Libnx, LibnxError_NotInitialized);
    }
    s_pctlSrv = *srv;
    s_initialized = true;
    return 0;
}

/* ------------------------------------------------------------------ */
/* pctl_exit                                                           */
/* ------------------------------------------------------------------ */
void pctl_exit(void)
{
    if (!s_initialized)
        return;

    pctlExit();
    s_initialized = false;
}

bool pctl_is_initialized(void)
{
    return s_initialized;
}

/* ------------------------------------------------------------------ */
/* Re-initialize pctl session (needed between certain calls)           */
/* ------------------------------------------------------------------ */
static Result pctl_reinit(void)
{
    pctl_exit();
    return pctl_init();
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

Result pctl_start_play_timer(void)
{
    Result rc = pctl_reinit();
    if (R_FAILED(rc)) return rc;
    return serviceDispatch(&s_pctlSrv, 1451);
}

Result pctl_stop_play_timer(void)
{
    Result rc = pctl_reinit();
    if (R_FAILED(rc)) return rc;
    return serviceDispatch(&s_pctlSrv, 1452);
}

Result pctl_is_enabled(bool *enabled)
{
    if (!enabled) return MAKERESULT(Module_Libnx, LibnxError_BadInput);

    Result rc = pctl_reinit();
    if (R_FAILED(rc)) return rc;

    u8 tmp = 0;
    rc = serviceDispatchOut(&s_pctlSrv, 1453, tmp);
    if (R_SUCCEEDED(rc))
        *enabled = (tmp != 0);
    return rc;
}

Result pctl_get_remaining_time(u64 *remaining_ns)
{
    if (!remaining_ns) return MAKERESULT(Module_Libnx, LibnxError_BadInput);

    Result rc = pctl_reinit();
    if (R_FAILED(rc)) return rc;

    /* serviceDispatchOut: 2nd arg is out var (by pointer), 
     * the macro fills *remaining_ns from IPC response */
    return serviceDispatchOut(&s_pctlSrv, 1454, *remaining_ns);
}

Result pctl_is_restricted(bool *restricted)
{
    if (!restricted) return MAKERESULT(Module_Libnx, LibnxError_BadInput);

    Result rc = pctl_reinit();
    if (R_FAILED(rc)) return rc;

    u8 tmp = 0;
    rc = serviceDispatchOut(&s_pctlSrv, 1455, tmp);
    if (R_SUCCEEDED(rc))
        *restricted = (tmp != 0);
    return rc;
}

/* ------------------------------------------------------------------ */
/* Settings read/write using HIPC pointer buffers                      */
/* ------------------------------------------------------------------ */

Result pctl_get_settings(PlayTimerSettings *settings)
{
    if (!settings) return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    memset(settings, 0, sizeof(*settings));

    Result rc = pctl_reinit();
    if (R_FAILED(rc)) return rc;

    /* GetPlayTimerSettings (cmd 145601):
     * Output: u16[34] via HIPC pointer buffer */
    return serviceDispatchOut(&s_pctlSrv, 145601, *settings,
        .buffer_attrs = { SfBufferAttr_HipcPointer | SfBufferAttr_Out },
        .buffers = { { settings, sizeof(PlayTimerSettings) } }
    );
}

Result pctl_set_settings(const PlayTimerSettings *settings)
{
    if (!settings) return MAKERESULT(Module_Libnx, LibnxError_BadInput);

    Result rc = pctl_reinit();
    if (R_FAILED(rc)) return rc;

    /* SetPlayTimerSettingsForDebug (cmd 1951):
     * Input: u16[34] via HIPC pointer buffer.
     * NOTE: Previously this was incorrectly written as 195101,
     * which is not a valid IPC command ID and caused
     * pctl 0xf601 errors on all write operations (SET, RESET). */
    return serviceDispatchIn(&s_pctlSrv, 1951, *settings,
        .buffer_attrs = { SfBufferAttr_HipcPointer | SfBufferAttr_In },
        .buffers = { { settings, sizeof(PlayTimerSettings) } }
    );
}

/* ------------------------------------------------------------------ */
/* Day-aware convenience functions                                     */
/* ------------------------------------------------------------------ */

Result pctl_get_day_limit_minutes(int day, u32 *minutes)
{
    if (!minutes || day < 0 || day > 7)
        return MAKERESULT(Module_Libnx, LibnxError_BadInput);

    PlayTimerSettings settings;
    Result rc = pctl_get_settings(&settings);
    if (R_FAILED(rc)) return rc;

    if (day == 7) {
        /* Return max minutes across all days */
        *minutes = 0;
        for (int d = 0; d < PCTL_DAYS; d++) {
            u16 m = settings.raw[PCTL_DAY_MINUTES_OFFSET(d)];
            if (m == PT_DAY_NOLIMIT) {
                *minutes = 0;  /* any day unlimited => report 0 */
                return 0;
            }
            if (m > *minutes) *minutes = m;
        }
    } else {
        u16 m = settings.raw[PCTL_DAY_MINUTES_OFFSET(day)];
        *minutes = (m == PT_DAY_NOLIMIT) ? 0 : m;
    }
    return 0;
}

Result pctl_set_day_limit_minutes(int day, u32 minutes)
{
    if (day < 0 || day >= PCTL_DAYS)
        return MAKERESULT(Module_Libnx, LibnxError_BadInput);

    PlayTimerSettings settings;
    Result rc = pctl_get_settings(&settings);
    if (R_FAILED(rc)) return rc;

    u16 val;
    if (minutes == 0) {
        val = PT_DAY_NOLIMIT;
    } else {
        if (minutes > 1440) minutes = 1440;
        val = (u16)minutes;
    }

    /* Set header if not already set */
    if (settings.raw[0] == 0) {
        settings.raw[0] = 0x0101;
        settings.raw[1] = 0x0001;
    }

    /* Set day flag + minutes */
    settings.raw[PCTL_DAY_FLAG_OFFSET(day)]    = (val != PT_DAY_NOLIMIT) ? 0x0100 : 0x0000;
    settings.raw[PCTL_DAY_MINUTES_OFFSET(day)] = val;

    return pctl_set_settings(&settings);
}

Result pctl_set_daily_limit_minutes(u32 minutes)
{
    PlayTimerSettings settings;
    Result rc = pctl_get_settings(&settings);
    if (R_FAILED(rc)) return rc;

    u16 val;
    if (minutes == 0) {
        val = PT_DAY_NOLIMIT;
    } else {
        if (minutes > 1440) minutes = 1440;
        val = (u16)minutes;
    }

    /* Set header */
    settings.raw[0] = (val != PT_DAY_NOLIMIT) ? 0x0101 : 0x0000;
    settings.raw[1] = (val != PT_DAY_NOLIMIT) ? 0x0001 : 0x0000;

    for (int d = 0; d < PCTL_DAYS; d++) {
        settings.raw[PCTL_DAY_FLAG_OFFSET(d)]    = (val != PT_DAY_NOLIMIT) ? 0x0100 : 0x0000;
        settings.raw[PCTL_DAY_MINUTES_OFFSET(d)] = val;
    }

    return pctl_set_settings(&settings);
}

Result pctl_get_daily_limit_minutes(u32 *minutes)
{
    return pctl_get_day_limit_minutes(0, minutes);
}

Result pctl_reset_play_time(void)
{
    Result rc;

    rc = pctl_stop_play_timer();
    if (R_FAILED(rc)) return rc;

    PlayTimerSettings settings;
    rc = pctl_get_settings(&settings);
    if (R_FAILED(rc)) return rc;

    rc = pctl_set_settings(&settings);
    if (R_FAILED(rc)) return rc;

    return pctl_start_play_timer();
}
