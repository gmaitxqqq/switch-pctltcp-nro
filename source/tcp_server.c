/**
 * tcp_server.c - TCP command server for pctltcp-nro
 *
 * Adapted from the sysmodule version for .nro (applet) context.
 * Key changes:
 *   - Runs in a dedicated pthread (not poll-based)
 *   - pctl is assumed already initialized by main.c before server starts
 *   - Thread-safe client counter for UI display
 *   - No console output (main.c handles the UI)
 */

#include "tcp_server.h"
#include "pctl_handler.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <poll.h>
#include <pthread.h>

/* ------------------------------------------------------------------ */
/* State                                                               */
/* ------------------------------------------------------------------ */
static int            s_server_fd   = -1;
static bool           s_running     = false;
static pthread_t      s_thread;
static atomic_uint    s_client_count = 0;

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */
static void send_str(int fd, const char *msg)
{
    if (fd >= 0 && msg)
        write(fd, msg, strlen(msg));
}

static void send_fmt(int fd, const char *fmt, ...)
{
    char buf[TCP_BUFFER_SIZE];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    send_str(fd, buf);
}

/* ------------------------------------------------------------------ */
/* Command handler                                                     */
/* ------------------------------------------------------------------ */
static void handle_command(int fd, const char *line)
{
    /* PING — no pctl needed */
    if (strncmp(line, "PING", 4) == 0) {
        send_fmt(fd, "PONG %d\n", 0);
        return;
    }

    /* VERSION — no pctl needed */
    if (strncmp(line, "VERSION", 7) == 0) {
        send_fmt(fd, "pctltcp-nro %s\n", VERSION_S);
        return;
    }

    /* All other commands need pctl — assume initialized by main.c */
    if (!pctl_is_initialized()) {
        send_str(fd, "ERR pctl not initialized\n");
        return;
    }

    /* GET — return current daily limit */
    if (strncmp(line, "GET", 3) == 0) {
        u32 minutes = 0;
        Result rc = pctl_get_daily_limit_minutes(&minutes);
        if (R_FAILED(rc)) {
            send_fmt(fd, "ERR pctl 0x%x\n", rc);
        } else {
            send_fmt(fd, "PLAYTIME %u\n", minutes);
        }
        return;
    }

    /* SET <minutes> — set all 7 days */
    if (strncmp(line, "SET ", 4) == 0) {
        u32 minutes = (u32)atoi(line + 4);
        Result rc = pctl_set_daily_limit_minutes(minutes);
        if (R_FAILED(rc)) {
            send_fmt(fd, "ERR pctl 0x%x\n", rc);
        } else {
            send_fmt(fd, "OK PLAYTIME %u\n", minutes);
        }
        return;
    }

    /* SET_DAY <day> <minutes> — set specific day */
    if (strncmp(line, "SET_DAY ", 8) == 0) {
        int day = -1;
        u32 minutes = 0;
        if (sscanf(line + 8, "%d %u", &day, &minutes) == 2 && day >= 0 && day <= 6) {
            Result rc = pctl_set_day_limit_minutes(day, minutes);
            if (R_FAILED(rc)) {
                send_fmt(fd, "ERR pctl 0x%x\n", rc);
            } else {
                send_fmt(fd, "OK DAY %d %u\n", day, minutes);
            }
        } else {
            send_str(fd, "ERR invalid SET_DAY format\n");
        }
        return;
    }

    /* START — start play timer */
    if (strncmp(line, "START", 5) == 0) {
        Result rc = pctl_start_play_timer();
        if (R_FAILED(rc)) {
            send_fmt(fd, "ERR pctl 0x%x\n", rc);
        } else {
            send_str(fd, "OK STARTED\n");
        }
        return;
    }

    /* STOP — stop play timer */
    if (strncmp(line, "STOP", 4) == 0) {
        Result rc = pctl_stop_play_timer();
        if (R_FAILED(rc)) {
            send_fmt(fd, "ERR pctl 0x%x\n", rc);
        } else {
            send_str(fd, "OK STOPPED\n");
        }
        return;
    }

    /* REMAINING — get remaining time */
    if (strncmp(line, "REMAINING", 9) == 0) {
        u64 remaining_ns = 0;
        Result rc = pctl_get_remaining_time(&remaining_ns);
        if (R_FAILED(rc)) {
            send_fmt(fd, "ERR pctl 0x%x\n", rc);
        } else {
            u32 rem_min = NS_TO_MINUTES(remaining_ns);
            send_fmt(fd, "REMAINING %u\n", rem_min);
        }
        return;
    }

    /* STATUS — get full status */
    if (strncmp(line, "STATUS", 6) == 0) {
        bool enabled = false, restricted = false;
        u64 remaining_ns = 0;
        u32 daily_limit = 0;

        Result rc1 = pctl_is_enabled(&enabled);
        Result rc2 = pctl_get_remaining_time(&remaining_ns);
        Result rc3 = pctl_is_restricted(&restricted);
        Result rc4 = pctl_get_daily_limit_minutes(&daily_limit);

        if (R_FAILED(rc1) || R_FAILED(rc2) || R_FAILED(rc3) || R_FAILED(rc4)) {
            send_fmt(fd, "ERR pctl 0x%x/0x%x/0x%x/0x%x\n", rc1, rc2, rc3, rc4);
        } else {
            u32 rem_min = NS_TO_MINUTES(remaining_ns);
            send_fmt(fd, "STATUS %s %u %u %s\n",
                     enabled ? "enabled" : "disabled",
                     rem_min,
                     daily_limit,
                     restricted ? "restricted" : "free");
        }
        return;
    }

    /* RESET — reset play time */
    if (strncmp(line, "RESET", 5) == 0) {
        Result rc = pctl_reset_play_time();
        if (R_FAILED(rc)) {
            send_fmt(fd, "ERR pctl 0x%x\n", rc);
        } else {
            send_str(fd, "OK RESET\n");
        }
        return;
    }

    /* Unknown command */
    send_str(fd, "ERR unknown command\n");
}

/* ------------------------------------------------------------------ */
/* Client handler — process one client connection (blocking)           */
/* ------------------------------------------------------------------ */
static void handle_client(int client_fd)
{
    char line[TCP_MAX_LINE];
    int line_pos = 0;

    while (s_running) {
        struct pollfd pfd;
        pfd.fd = client_fd;
        pfd.events = POLLIN;
        pfd.revents = 0;

        int ret = poll(&pfd, 1, 5000);  /* 5s timeout */
        if (ret < 0) break;
        if (ret == 0) continue;  /* timeout, keep waiting */

        if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL))
            break;

        if (pfd.revents & POLLIN) {
            char buf[128];
            ssize_t n = read(client_fd, buf, sizeof(buf));
            if (n <= 0) break;

            for (ssize_t i = 0; i < n; i++) {
                if (buf[i] == '\n' || buf[i] == '\r') {
                    if (line_pos > 0) {
                        line[line_pos] = '\0';
                        handle_command(client_fd, line);
                        line_pos = 0;
                    }
                } else if (line_pos < TCP_MAX_LINE - 1) {
                    line[line_pos++] = buf[i];
                }
            }
        }
    }

    close(client_fd);
}

/* ------------------------------------------------------------------ */
/* Server thread — accept loop                                         */
/* ------------------------------------------------------------------ */
static void *server_thread_func(void *arg)
{
    (void)arg;

    while (s_running) {
        struct pollfd pfd;
        pfd.fd = s_server_fd;
        pfd.events = POLLIN;
        pfd.revents = 0;

        int ret = poll(&pfd, 1, 500);  /* 500ms poll */
        if (ret <= 0)
            continue;

        if (pfd.revents & POLLIN) {
            int client_fd = accept(s_server_fd, NULL, NULL);
            if (client_fd >= 0) {
                atomic_fetch_add(&s_client_count, 1);
                handle_client(client_fd);
                atomic_fetch_sub(&s_client_count, 1);
            }
        }
    }

    return NULL;
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

Result tcp_server_start(void)
{
    struct sockaddr_in addr;

    s_server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (s_server_fd < 0)
        return MAKERESULT(Module_Libnx, LibnxError_IoError);

    int optval = 1;
    setsockopt(s_server_fd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));

    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(TCP_PORT);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(s_server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(s_server_fd);
        s_server_fd = -1;
        return MAKERESULT(Module_Libnx, LibnxError_IoError);
    }

    if (listen(s_server_fd, 2) < 0) {
        close(s_server_fd);
        s_server_fd = -1;
        return MAKERESULT(Module_Libnx, LibnxError_IoError);
    }

    s_running = true;

    /* Launch accept thread */
    if (pthread_create(&s_thread, NULL, server_thread_func, NULL) != 0) {
        s_running = false;
        close(s_server_fd);
        s_server_fd = -1;
        return MAKERESULT(Module_Libnx, LibnxError_IoError);
    }

    return 0;
}

void tcp_server_stop(void)
{
    s_running = false;

    if (s_server_fd >= 0) {
        close(s_server_fd);
        s_server_fd = -1;
    }

    /* Wait for thread to finish */
    pthread_join(s_thread, NULL);
}

bool tcp_server_is_running(void)
{
    return s_running;
}

u32 tcp_server_client_count(void)
{
    return atomic_load(&s_client_count);
}
