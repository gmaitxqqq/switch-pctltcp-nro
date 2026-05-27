/**
 * tcp_server.h - TCP server for pctltcp-nro
 *
 * Lightweight TCP server that listens on port 6000 and processes
 * text-based play timer commands from the PC client.
 *
 * Runs in a dedicated pthread — does NOT depend on appletMainLoop().
 * Designed for .nro (user-mode applet) context where pctlInitialize()
 * succeeds and all IPC commands work.
 */

#ifndef TCP_SERVER_H
#define TCP_SERVER_H

#include <switch.h>

#define TCP_PORT           6000
#define TCP_BUFFER_SIZE    512
#define TCP_MAX_LINE       256
#define VERSION_S          "1.5.0"

/** Start the TCP server and launch the accept thread. Returns 0 on success. */
Result tcp_server_start(void);

/** Stop the server thread and close all sockets. */
void tcp_server_stop(void);

/** Check if the server is currently running. */
bool tcp_server_is_running(void);

/** Get number of clients currently connected (for UI display). */
u32 tcp_server_client_count(void);

/** Get the IP address the server is bound to (for display). */
const char *tcp_server_get_ip(void);

#endif /* TCP_SERVER_H */
