#ifndef UNBRIDLE_H
#define UNBRIDLE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <sys/socket.h>

#define UNBRIDLE_CONFIG_FILE ".config/unbridle/unbridle.ini"
#define UNBRIDLE_PACKET_FILE ".config/unbridle/unbridle-packet.bin"
#define UNBRIDLE_VERSION "3.0.0-linux"

// Proxy types
typedef enum {
    PROXY_TYPE_NONE = 0,
    PROXY_TYPE_HTTP,
    PROXY_TYPE_SOCKS5
} proxy_type_t;

// Proxy configuration
typedef struct {
    bool enabled;
    proxy_type_t type;
    char host[256];
    int port;
    char username[128];
    char password[128];
    bool has_auth;
} proxy_config_t;

// Socket tracking
typedef struct {
    int fd;
    bool is_tcp;
    bool is_udp;
    bool has_sent;
    bool voice_manipulated;
    bool fake_http_proxy;
    uint64_t created_at;
} socket_info_t;

// Configuration functions
bool load_config(proxy_config_t *config);
bool save_config(const proxy_config_t *config);
bool save_raw_proxy_line(const char *raw_proxy_url); /* used by the minimal single-field UI */

// Socket management
void socket_manager_init(void);
void socket_manager_cleanup(void);
void socket_manager_add(int fd, int type, int protocol);
socket_info_t* socket_manager_find(int fd);
bool socket_manager_is_first_send(int fd, socket_info_t **info);
bool socket_manager_mark_voice_manipulated(int fd, socket_info_t **info);
void socket_manager_set_fake_http_flag(int fd);
bool socket_manager_reset_fake_http_flag(int fd);

// Proxy functions
bool convert_http_to_socks5(socket_info_t *sock_info, const void *buf, size_t len);
bool add_http_proxy_auth_header(socket_info_t *sock_info, void *buf, size_t *len);
void inject_udp_manipulation(int fd, const struct sockaddr *dest_addr, socklen_t addrlen);

/*
 * looks_like_voice_ip_discovery - Check whether a 74-byte UDP payload matches
 * Discord's documented Voice IP Discovery packet header (Type=0x0001,
 * Length=0x0046, i.e. bytes {0x00,0x01,0x00,0x46,...}), not just its length.
 *
 * Length alone (74 bytes) is not a strong enough signal - any other 74-byte
 * UDP send on the same socket would also match, and injecting bytes ahead of
 * unrelated traffic (rather than a genuine voice packet) is exactly the kind
 * of thing that can desync a session. This checks the actual header fields
 * of the real, documented Discord voice protocol before we touch anything.
 */
bool looks_like_voice_ip_discovery(const void *buf, size_t len);

// Utility functions
uint64_t get_time_ms(void);
char* get_home_dir(void);
char* get_config_path(void);

#endif // UNBRIDLE_H
