/*
 * hook.c - Main LD_PRELOAD hook implementation
 *
 * Intercepts socket-related libc functions to redirect Discord's network
 * traffic through a configured proxy server (HTTP or SOCKS5), and to
 * inject the voice-bypass UDP manipulation used in Direct Mode.
 *
 * Hooked functions:
 * - socket()   - Track new sockets
 * - connect()  - Redirect TCP connections to proxy
 * - send()     - Inject SOCKS5 negotiation
 * - recv()     - Return fake HTTP response for SOCKS5
 * - sendto()   - Inject UDP manipulation packets
 * - recvfrom() - Passthrough (for completeness)
 *
 * Thread-safe initialization with pthread_once.
 * Configuration loaded from ~/.config/unbridle/unbridle.ini
 *
 * IMPORTANT: This library deliberately does NOT clear LD_PRELOAD from its
 * own environment after loading. Discord on Linux is a multi-process
 * Electron/Chromium app - the window you interact with is one process, but
 * networking (including voice UDP) commonly happens in separate child
 * processes spawned by re-exec'ing the Discord binary. LD_PRELOAD is only
 * consulted by the dynamic linker at exec() time, so stripping it here -
 * before any children exist - would mean those children never load this
 * hook at all, even though this top-level process starts up fine. Leaving
 * it set is required for the hook to reach the process actually doing the
 * socket work, and costs nothing: Discord has no reason to audit its own
 * environment.
 */

#define _GNU_SOURCE
#include "unbridle.h"
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <pthread.h>
#include <errno.h>

/* Original function pointers - resolved at init time */
int (*real_socket)(int domain, int type, int protocol) = NULL;
int (*real_connect)(int sockfd, const struct sockaddr *addr, socklen_t addrlen) = NULL;
ssize_t (*real_send)(int sockfd, const void *buf, size_t len, int flags) = NULL;
ssize_t (*real_recv)(int sockfd, void *buf, size_t len, int flags) = NULL;
ssize_t (*real_sendto)(int sockfd, const void *buf, size_t len, int flags,
                       const struct sockaddr *dest_addr, socklen_t addrlen) = NULL;
ssize_t (*real_recvfrom)(int sockfd, void *buf, size_t len, int flags,
                         struct sockaddr *src_addr, socklen_t *addrlen) = NULL;
ssize_t (*real_sendmsg)(int sockfd, const struct msghdr *msg, int flags) = NULL;

/* Global proxy configuration - loaded at init */
proxy_config_t g_proxy_config = {0};
static pthread_once_t init_once = PTHREAD_ONCE_INIT;

static void init_hook(void) {
    real_socket = dlsym(RTLD_NEXT, "socket");
    real_connect = dlsym(RTLD_NEXT, "connect");
    real_send = dlsym(RTLD_NEXT, "send");
    real_recv = dlsym(RTLD_NEXT, "recv");
    real_sendto = dlsym(RTLD_NEXT, "sendto");
    real_recvfrom = dlsym(RTLD_NEXT, "recvfrom");
    real_sendmsg = dlsym(RTLD_NEXT, "sendmsg");

    if (!real_socket || !real_connect || !real_send || !real_recv ||
        !real_sendto || !real_recvfrom) {
        fprintf(stderr, "[unbridle] ERROR: Failed to resolve socket functions\n");
        return;
    }

    socket_manager_init();

    if (!load_config(&g_proxy_config)) {
        fprintf(stderr, "[unbridle] WARNING: Failed to load config, using defaults\n");
    }

    fprintf(stderr, "[unbridle] Initialized v%s (pid %d)\n", UNBRIDLE_VERSION, getpid());
    fprintf(stderr, "[unbridle] Proxy: %s", g_proxy_config.enabled ? "enabled" : "disabled (direct mode)");
    if (g_proxy_config.enabled) {
        fprintf(stderr, " | Type: %s | Host: %s:%d\n",
                g_proxy_config.type == PROXY_TYPE_SOCKS5 ? "SOCKS5" : "HTTP",
                g_proxy_config.host, g_proxy_config.port);
    } else {
        fprintf(stderr, "\n");
    }
}

int socket(int domain, int type, int protocol) {
    pthread_once(&init_once, init_hook);

    if (!real_socket) {
        errno = ENOSYS;
        return -1;
    }

    int fd = real_socket(domain, type, protocol);
    if (fd >= 0) {
        int clean_type = type & ~SOCK_NONBLOCK & ~SOCK_CLOEXEC;
        socket_manager_add(fd, clean_type, protocol);
    }
    return fd;
}

int connect(int sockfd, const struct sockaddr *addr, socklen_t addrlen) {
    pthread_once(&init_once, init_hook);

    if (!real_connect) {
        errno = ENOSYS;
        return -1;
    }

    socket_info_t *sock_info = socket_manager_find(sockfd);

    if (g_proxy_config.enabled && sock_info && sock_info->is_tcp) {
        struct sockaddr_in proxy_addr;
        memset(&proxy_addr, 0, sizeof(proxy_addr));
        proxy_addr.sin_family = AF_INET;
        proxy_addr.sin_port = htons(g_proxy_config.port);

        if (inet_pton(AF_INET, g_proxy_config.host, &proxy_addr.sin_addr) == 1) {
            return real_connect(sockfd, (struct sockaddr *)&proxy_addr, sizeof(proxy_addr));
        } else {
            fprintf(stderr, "[unbridle] WARNING: Invalid proxy host '%s', using direct connection\n",
                    g_proxy_config.host);
        }
    }

    return real_connect(sockfd, addr, addrlen);
}

ssize_t send(int sockfd, const void *buf, size_t len, int flags) {
    pthread_once(&init_once, init_hook);

    if (!real_send) {
        errno = ENOSYS;
        return -1;
    }

    socket_info_t *sock_info;
    if (socket_manager_is_first_send(sockfd, &sock_info)) {
        if (g_proxy_config.enabled && sock_info->is_tcp) {
            if (convert_http_to_socks5(sock_info, buf, len)) {
                return len;
            }
        }
    }

    return real_send(sockfd, buf, len, flags);
}

ssize_t recv(int sockfd, void *buf, size_t len, int flags) {
    pthread_once(&init_once, init_hook);

    if (!real_recv) {
        errno = ENOSYS;
        return -1;
    }

    ssize_t result = real_recv(sockfd, buf, len, flags);

    if (result > 0 && socket_manager_reset_fake_http_flag(sockfd)) {
        if (result >= 10) {
            char *data = (char *)buf;
            if (data[0] == 0x05 && data[1] == 0x00 && data[2] == 0x00) {
                const char *fake_response = "HTTP/1.1 200 Connection Established\r\n\r\n";
                size_t fake_len = strlen(fake_response);
                if (fake_len <= len) {
                    memcpy(buf, fake_response, fake_len);
                    return fake_len;
                }
            }
        }
    }

    return result;
}

/*
 * sendto() hook - Inject UDP manipulation packets
 *
 * Only fires on the FIRST sendto() for a UDP socket, and only when the
 * payload actually matches Discord's documented Voice IP Discovery packet
 * header (checked in looks_like_voice_ip_discovery(), not just its length).
 * That distinction matters: a length-only check would also fire on any
 * other 74-byte UDP send this socket happens to make, and injecting bytes
 * ahead of traffic that isn't actually voice setup is how you end up
 * corrupting something Discord's servers care about.
 */
ssize_t sendmsg(int sockfd, const struct msghdr *msg, int flags) {
    pthread_once(&init_once, init_hook);
    if (!real_sendmsg) { errno = ENOSYS; return -1; }
    if (msg && msg->msg_name && msg->msg_namelen > 0 && msg->msg_iov && msg->msg_iovlen > 0) {
        unsigned char packet[74]; size_t copied = 0;
        for (size_t i = 0; i < msg->msg_iovlen && copied < sizeof(packet); i++) {
            size_t n = msg->msg_iov[i].iov_len;
            if (n > sizeof(packet) - copied) n = sizeof(packet) - copied;
            memcpy(packet + copied, msg->msg_iov[i].iov_base, n); copied += n;
        }
        if (copied == sizeof(packet) && looks_like_voice_ip_discovery(packet, copied)) {
            socket_info_t *info = NULL;
            if (socket_manager_mark_voice_manipulated(sockfd, &info)) {
                fprintf(stderr, "[unbridle] Voice IP Discovery detected via sendmsg (fd=%d)\n", sockfd);
                inject_udp_manipulation(sockfd, msg->msg_name, msg->msg_namelen);
            }
        }
    }
    return real_sendmsg(sockfd, msg, flags);
}

ssize_t sendto(int sockfd, const void *buf, size_t len, int flags,
               const struct sockaddr *dest_addr, socklen_t addrlen) {
    pthread_once(&init_once, init_hook);

    if (!real_sendto) {
        errno = ENOSYS;
        return -1;
    }

    socket_info_t *sock_info = NULL;
    if (dest_addr && looks_like_voice_ip_discovery(buf, len) &&
        socket_manager_mark_voice_manipulated(sockfd, &sock_info)) {
        fprintf(stderr, "[unbridle] Voice IP Discovery detected via sendto (fd=%d)\n", sockfd);
        inject_udp_manipulation(sockfd, dest_addr, addrlen);
    }

    return real_sendto(sockfd, buf, len, flags, dest_addr, addrlen);
}

ssize_t recvfrom(int sockfd, void *buf, size_t len, int flags,
                 struct sockaddr *src_addr, socklen_t *addrlen) {
    pthread_once(&init_once, init_hook);

    if (!real_recvfrom) {
        errno = ENOSYS;
        return -1;
    }

    return real_recvfrom(sockfd, buf, len, flags, src_addr, addrlen);
}

__attribute__((constructor))
static void unbridle_init(void) {
    pthread_once(&init_once, init_hook);
}

__attribute__((destructor))
static void unbridle_fini(void) {
    socket_manager_cleanup();
}
