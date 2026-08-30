/*
 * socket_manager.c - Thread-safe socket tracking for Discord proxy interception
 *
 * Manages a table of active sockets to track their state:
 * - Socket type (TCP/UDP)
 * - First send detection (for proxy negotiation)
 * - SOCKS5 fake HTTP response flag
 *
 * Thread-safe with mutex protection for all operations.
 * Automatic garbage collection removes stale entries.
 */

#include "unbridle.h"
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>

#define MAX_SOCKETS 1024          /* Maximum tracked sockets - increase if needed */
#define SOCKET_TIMEOUT_MS 30000   /* Auto-remove sockets inactive for 30 seconds */

/* Global socket table - protected by mutex */
static socket_info_t sockets[MAX_SOCKETS];
static int socket_count = 0;
static pthread_mutex_t socket_mutex = PTHREAD_MUTEX_INITIALIZER;

/*
 * socket_manager_init - Initialize the socket manager
 *
 * Called once at library load time. Safe to call multiple times.
 */
void socket_manager_init(void) {
    pthread_mutex_lock(&socket_mutex);
    memset(sockets, 0, sizeof(sockets));
    socket_count = 0;
    pthread_mutex_unlock(&socket_mutex);
}

/*
 * socket_manager_cleanup - Clean up socket manager resources
 *
 * Called at library unload. Safe to call multiple times.
 */
void socket_manager_cleanup(void) {
    pthread_mutex_lock(&socket_mutex);
    memset(sockets, 0, sizeof(sockets));
    socket_count = 0;
    pthread_mutex_unlock(&socket_mutex);
}

/*
 * collect_garbage - Remove stale socket entries
 *
 * MUST be called with socket_mutex held.
 * Removes sockets that haven't been updated in SOCKET_TIMEOUT_MS.
 * This prevents the table from filling with closed/abandoned sockets.
 */
static void collect_garbage(void) {
    uint64_t now = get_time_ms();
    uint64_t cutoff = now - SOCKET_TIMEOUT_MS;

    /* Iterate backwards for safe removal */
    for (int i = socket_count - 1; i >= 0; i--) {
        if (sockets[i].created_at < cutoff) {
            /* Remove by swapping with last element */
            if (i < socket_count - 1) {
                sockets[i] = sockets[socket_count - 1];
            }
            socket_count--;
        }
    }
}

/*
 * socket_manager_add - Add or update a socket in the tracking table
 *
 * @fd: Socket file descriptor
 * @type: Socket type (SOCK_STREAM, SOCK_DGRAM, etc.)
 * @protocol: Protocol (IPPROTO_TCP, IPPROTO_UDP, etc.)
 *
 * Called when socket() or WSASocket() is intercepted.
 * Updates existing entry if socket FD already tracked (handles socket reuse).
 * Automatically runs garbage collection to free space.
 */
void socket_manager_add(int fd, int type, int protocol) {
    if (fd < 0) return; /* Invalid socket */

    pthread_mutex_lock(&socket_mutex);

    collect_garbage();

    /* Check if socket already exists (FD reuse) */
    int idx = -1;
    for (int i = 0; i < socket_count; i++) {
        if (sockets[i].fd == fd) {
            idx = i;
            break;
        }
    }

    /* Add new entry or update existing */
    if (idx == -1) {
        if (socket_count >= MAX_SOCKETS) {
            /* Table full - silently drop (garbage collection will make space later) */
            pthread_mutex_unlock(&socket_mutex);
            return;
        }
        idx = socket_count++;
    }

    /* Initialize socket info */
    sockets[idx].fd = fd;
    sockets[idx].is_tcp = (type == SOCK_STREAM && (protocol == IPPROTO_TCP || protocol == 0));
    sockets[idx].is_udp = (type == SOCK_DGRAM && (protocol == IPPROTO_UDP || protocol == 0));
    sockets[idx].has_sent = false;
    sockets[idx].voice_manipulated = false;
    sockets[idx].fake_http_proxy = false;
    sockets[idx].created_at = get_time_ms();

    pthread_mutex_unlock(&socket_mutex);
}

/*
 * socket_manager_find - Find socket info by file descriptor
 *
 * @fd: Socket file descriptor to look up
 *
 * Returns: Pointer to socket info, or NULL if not found
 *
 * WARNING: Returned pointer is only valid until next manager operation.
 *          Do not store for later use. Copy data if needed across calls.
 */
socket_info_t* socket_manager_find(int fd) {
    pthread_mutex_lock(&socket_mutex);

    for (int i = 0; i < socket_count; i++) {
        if (sockets[i].fd == fd) {
            socket_info_t *result = &sockets[i];
            pthread_mutex_unlock(&socket_mutex);
            return result;
        }
    }

    pthread_mutex_unlock(&socket_mutex);
    return NULL;
}

/*
 * socket_manager_is_first_send - Check and mark if this is the first send on socket
 *
 * @fd: Socket file descriptor
 * @info: Output pointer to socket info (set only if returns true)
 *
 * Returns: true if this is the first send, false otherwise
 *
 * Used to detect when to inject proxy negotiation or UDP manipulation.
 * Atomically checks and sets the has_sent flag.
 *
 * WARNING: Returned info pointer only valid until next manager operation.
 */
bool socket_manager_mark_voice_manipulated(int fd, socket_info_t **info) {
    if (!info) return false;
    pthread_mutex_lock(&socket_mutex);
    for (int i = 0; i < socket_count; i++) {
        if (sockets[i].fd == fd) {
            if (!sockets[i].is_udp || sockets[i].voice_manipulated) {
                pthread_mutex_unlock(&socket_mutex);
                return false;
            }
            sockets[i].voice_manipulated = true;
            *info = &sockets[i];
            pthread_mutex_unlock(&socket_mutex);
            return true;
        }
    }
    pthread_mutex_unlock(&socket_mutex);
    return false;
}

bool socket_manager_is_first_send(int fd, socket_info_t **info) {
    if (!info) return false;

    pthread_mutex_lock(&socket_mutex);

    for (int i = 0; i < socket_count; i++) {
        if (sockets[i].fd == fd) {
            if (sockets[i].has_sent) {
                pthread_mutex_unlock(&socket_mutex);
                return false;
            }
            /* Mark as sent and return info */
            sockets[i].has_sent = true;
            *info = &sockets[i];
            pthread_mutex_unlock(&socket_mutex);
            return true;
        }
    }

    pthread_mutex_unlock(&socket_mutex);
    return false;
}

/*
 * socket_manager_set_fake_http_flag - Mark socket as needing fake HTTP response
 *
 * @fd: Socket file descriptor
 *
 * Used when we send a SOCKS5 request but need to return a fake HTTP 200
 * response to the application (SOCKS5 proxy conversion).
 */
void socket_manager_set_fake_http_flag(int fd) {
    pthread_mutex_lock(&socket_mutex);

    for (int i = 0; i < socket_count; i++) {
        if (sockets[i].fd == fd) {
            sockets[i].fake_http_proxy = true;
            break;
        }
    }

    pthread_mutex_unlock(&socket_mutex);
}

/*
 * socket_manager_reset_fake_http_flag - Check and clear fake HTTP response flag
 *
 * @fd: Socket file descriptor
 *
 * Returns: true if flag was set (and has now been cleared), false otherwise
 *
 * Called on recv() to determine if we should replace SOCKS5 response
 * with fake HTTP 200. Atomically checks and clears the flag.
 */
bool socket_manager_reset_fake_http_flag(int fd) {
    pthread_mutex_lock(&socket_mutex);

    for (int i = 0; i < socket_count; i++) {
        if (sockets[i].fd == fd) {
            if (!sockets[i].fake_http_proxy) {
                pthread_mutex_unlock(&socket_mutex);
                return false;
            }
            /* Clear flag and return true */
            sockets[i].fake_http_proxy = false;
            pthread_mutex_unlock(&socket_mutex);
            return true;
        }
    }

    pthread_mutex_unlock(&socket_mutex);
    return false;
}
