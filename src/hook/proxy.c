/*
 * proxy.c - Proxy protocol conversion and authentication
 *
 * Handles two main functions:
 * 1. HTTP CONNECT → SOCKS5 protocol conversion
 * 2. HTTP Proxy-Authorization header injection (for auth)
 *
 * When Discord sends HTTP CONNECT through a SOCKS5 proxy, we convert it
 * to proper SOCKS5 protocol and fake an HTTP response back to Discord.
 */

#define _GNU_SOURCE
#include "unbridle.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/select.h>
#include <regex.h>
#include <ctype.h>
#include <errno.h>

/* External references from hook.c */
extern ssize_t (*real_send)(int sockfd, const void *buf, size_t len, int flags);
extern ssize_t (*real_recv)(int sockfd, void *buf, size_t len, int flags);
extern proxy_config_t g_proxy_config;

/* Base64 encoding table for HTTP authentication */
static const char base64_table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/*
 * base64_encode - Encode binary data to Base64
 *
 * @input: Binary data to encode
 * @len: Length of input data
 * @output: Output buffer (must be large enough: ((len + 2) / 3) * 4 + 1 bytes)
 *
 * Standard RFC 4648 Base64 encoding with padding.
 * Used for HTTP Proxy-Authorization header.
 */
static void base64_encode(const unsigned char *input, size_t len, char *output) {
    size_t i = 0, j = 0;
    unsigned char a3[3], a4[4];
    size_t k = 0;

    while (len--) {
        a3[i++] = *(input++);
        if (i == 3) {
            /* Encode 3 bytes into 4 Base64 characters */
            a4[0] = (a3[0] & 0xfc) >> 2;
            a4[1] = ((a3[0] & 0x03) << 4) + ((a3[1] & 0xf0) >> 4);
            a4[2] = ((a3[1] & 0x0f) << 2) + ((a3[2] & 0xc0) >> 6);
            a4[3] = a3[2] & 0x3f;

            for (i = 0; i < 4; i++) {
                output[j++] = base64_table[a4[i]];
            }
            i = 0;
        }
    }

    /* Handle remaining bytes (padding) */
    if (i) {
        for (k = i; k < 3; k++) {
            a3[k] = '\0';
        }

        a4[0] = (a3[0] & 0xfc) >> 2;
        a4[1] = ((a3[0] & 0x03) << 4) + ((a3[1] & 0xf0) >> 4);
        a4[2] = ((a3[1] & 0x0f) << 2) + ((a3[2] & 0xc0) >> 6);
        a4[3] = a3[2] & 0x3f;

        for (k = 0; k < i + 1; k++) {
            output[j++] = base64_table[a4[k]];
        }

        /* Add padding '=' characters */
        while (i++ < 3) {
            output[j++] = '=';
        }
    }
    output[j] = '\0';
}

/*
 * add_http_proxy_auth_header - Inject Proxy-Authorization header into HTTP request
 *
 * @sock_info: Socket information
 * @buf: HTTP request buffer (modified in-place)
 * @len: Pointer to buffer length (unused, for future expansion)
 *
 * Returns: true if header was injected, false otherwise
 *
 * NOTE: This function is currently unused but kept for future HTTP auth support.
 * Discord doesn't send HTTP CONNECT when using HTTP proxy, so this code path
 * is not exercised. Kept for completeness and potential future use.
 *
 * Strategy: Replace User-Agent header with Proxy-Authorization + padding
 * to avoid changing packet size (some proxies are sensitive to this).
 */
bool add_http_proxy_auth_header(socket_info_t *sock_info, void *buf, size_t *len) {
    /* Validate preconditions */
    if (!sock_info || !buf || !len) return false;
    if (!g_proxy_config.enabled || g_proxy_config.type != PROXY_TYPE_HTTP) return false;
    if (!g_proxy_config.has_auth || !sock_info->is_tcp) return false;

    char *data = (char *)buf;

    /* Check if Proxy-Authorization already exists (avoid duplicate) */
    if (strstr(data, "\r\nProxy-Authorization: ") != NULL) {
        return false;
    }

    /* Find User-Agent header to replace (Discord always sends this) */
    char *ua_start = strstr(data, "User-Agent:");
    if (!ua_start) return false;

    char *ua_end = strstr(ua_start, "\r\n");
    if (!ua_end) return false;

    size_t ua_len = ua_end - ua_start;

    /* Build authentication string: username:password */
    char auth_str[256];
    int auth_str_len = snprintf(auth_str, sizeof(auth_str), "%s:%s",
                                 g_proxy_config.username, g_proxy_config.password);
    if (auth_str_len < 0 || auth_str_len >= (int)sizeof(auth_str)) {
        return false; /* Username/password too long */
    }

    /* Encode to Base64 */
    char auth_b64[512];
    base64_encode((unsigned char *)auth_str, strlen(auth_str), auth_b64);

    /* Build Proxy-Authorization header */
    char proxy_auth[768];
    int proxy_auth_len = snprintf(proxy_auth, sizeof(proxy_auth),
                                   "Proxy-Authorization: Basic %s", auth_b64);
    if (proxy_auth_len < 0 || proxy_auth_len >= (int)sizeof(proxy_auth)) {
        return false;
    }

    /* Add padding to match User-Agent length (avoid changing packet size) */
    if (proxy_auth_len < (int)ua_len) {
        int filler_len = ua_len - proxy_auth_len;
        if (filler_len >= 6 && sizeof(proxy_auth) - proxy_auth_len > (size_t)filler_len) {
            strncat(proxy_auth, "\r\nX: ", sizeof(proxy_auth) - strlen(proxy_auth) - 1);
            /* Fill with 'X' characters */
            int x_count = filler_len - 5;
            for (int i = 0; i < x_count && strlen(proxy_auth) < sizeof(proxy_auth) - 1; i++) {
                strncat(proxy_auth, "X", sizeof(proxy_auth) - strlen(proxy_auth) - 1);
            }
        }
    }

    /* Replace User-Agent with Proxy-Authorization (if exact size match) */
    if (strlen(proxy_auth) == ua_len) {
        memcpy(ua_start, proxy_auth, ua_len);
        return true;
    }

    return false;
}

/*
 * convert_http_to_socks5 - Convert HTTP CONNECT request to SOCKS5 protocol
 *
 * @sock_info: Socket information
 * @buf: Buffer containing HTTP CONNECT request
 * @len: Length of buffer
 *
 * Returns: true if conversion succeeded, false otherwise
 *
 * When Discord sends "CONNECT host:port HTTP/1.1", we:
 * 1. Parse the target host and port
 * 2. Send SOCKS5 handshake
 * 3. Send SOCKS5 CONNECT request
 * 4. Mark socket to return fake HTTP response later
 *
 * Discord will then receive "HTTP/1.1 200 Connection Established" (faked)
 * and proceed with the connection as if it was an HTTP proxy.
 */
bool convert_http_to_socks5(socket_info_t *sock_info, const void *buf, size_t len) {
    /* Validate preconditions */
    if (!sock_info || !buf) return false;
    if (!g_proxy_config.enabled || g_proxy_config.type != PROXY_TYPE_SOCKS5) return false;
    if (!sock_info->is_tcp) return false;

    const char *data = (const char *)buf;

    /* Check for HTTP CONNECT request */
    if (len < 8 || strncmp(data, "CONNECT ", 8) != 0) {
        return false;
    }

    /* Parse CONNECT target using regex: CONNECT host:port HTTP/1.x */
    regex_t regex;
    regmatch_t matches[3];

    /* Compile regex (allow letters, numbers, dots, hyphens in hostname) */
    if (regcomp(&regex, "^CONNECT ([a-zA-Z0-9.-]+):([0-9]+)", REG_EXTENDED) != 0) {
        fprintf(stderr, "[discord-unbridle] ERROR: Failed to compile regex\n");
        return false;
    }

    int ret = regexec(&regex, data, 3, matches, 0);
    regfree(&regex);

    if (ret != 0) {
        return false; /* Not a valid CONNECT request */
    }

    /* Extract target host and port from regex matches */
    char target_host[256];
    char target_port_str[16];

    int host_len = matches[1].rm_eo - matches[1].rm_so;
    int port_len = matches[2].rm_eo - matches[2].rm_so;

    /* Bounds checking */
    if (host_len >= (int)sizeof(target_host) || port_len >= (int)sizeof(target_port_str)) {
        return false;
    }

    strncpy(target_host, data + matches[1].rm_so, host_len);
    target_host[host_len] = '\0';

    strncpy(target_port_str, data + matches[2].rm_so, port_len);
    target_port_str[port_len] = '\0';

    int port_int = atoi(target_port_str);
    if (port_int <= 0 || port_int > 65535) {
        return false; /* Invalid port */
    }
    uint16_t target_port = (uint16_t)port_int;

    /*
     * SOCKS5 Handshake Phase
     * Send: [0x05] [0x01] [0x00]
     *       version  nmethods  no-auth
     */
    char handshake[] = {0x05, 0x01, 0x00};
    if (real_send(sock_info->fd, handshake, 3, 0) != 3) {
        fprintf(stderr, "[discord-unbridle] ERROR: Failed to send SOCKS5 handshake\n");
        return false;
    }

    /* Wait for SOCKS5 server response (with timeout) */
    fd_set readfds;
    struct timeval tv;
    FD_ZERO(&readfds);
    FD_SET(sock_info->fd, &readfds);
    tv.tv_sec = 10;  /* 10 second timeout */
    tv.tv_usec = 0;

    if (select(sock_info->fd + 1, &readfds, NULL, NULL, &tv) <= 0) {
        fprintf(stderr, "[discord-unbridle] ERROR: SOCKS5 handshake timeout\n");
        return false;
    }

    /* Read SOCKS5 handshake response: [version] [method] */
    char response[2];
    if (real_recv(sock_info->fd, response, 2, 0) != 2) {
        fprintf(stderr, "[discord-unbridle] ERROR: Failed to read SOCKS5 handshake response\n");
        return false;
    }

    /* Verify response: version=5, method=no-auth(0) */
    if (response[0] != 0x05 || response[1] != 0x00) {
        fprintf(stderr, "[discord-unbridle] ERROR: SOCKS5 handshake failed (version=%d, method=%d)\n",
                response[0], response[1]);
        return false;
    }

    /*
     * SOCKS5 CONNECT Request
     * Format: [VER][CMD][RSV][ATYP][DST.ADDR][DST.PORT]
     * - VER: 0x05 (SOCKS5)
     * - CMD: 0x01 (CONNECT)
     * - RSV: 0x00 (reserved)
     * - ATYP: 0x03 (domain name)
     * - DST.ADDR: [length byte][domain bytes]
     * - DST.PORT: 2 bytes, big-endian
     */
    size_t host_len_byte = strlen(target_host);
    if (host_len_byte > 255) {
        fprintf(stderr, "[discord-unbridle] ERROR: Hostname too long\n");
        return false;
    }

    size_t request_len = 7 + host_len_byte;
    char *request = malloc(request_len);
    if (!request) {
        fprintf(stderr, "[discord-unbridle] ERROR: Failed to allocate SOCKS5 request\n");
        return false;
    }

    request[0] = 0x05; /* SOCKS version */
    request[1] = 0x01; /* CONNECT command */
    request[2] = 0x00; /* Reserved */
    request[3] = 0x03; /* Address type: domain name */
    request[4] = (char)host_len_byte; /* Domain length */
    memcpy(request + 5, target_host, host_len_byte);
    request[5 + host_len_byte] = (target_port >> 8) & 0xFF; /* Port high byte */
    request[6 + host_len_byte] = target_port & 0xFF;        /* Port low byte */

    ssize_t sent = real_send(sock_info->fd, request, request_len, 0);
    free(request);

    if (sent != (ssize_t)request_len) {
        fprintf(stderr, "[discord-unbridle] ERROR: Failed to send SOCKS5 CONNECT request\n");
        return false;
    }

    /*
     * Mark this socket to return a fake HTTP response on next recv()
     * Discord expects "HTTP/1.1 200 Connection Established\r\n\r\n"
     * but SOCKS5 server will send binary response
     */
    socket_manager_set_fake_http_flag(sock_info->fd);

    return true;
}
