/*
 * udp_manipulation.c - UDP packet injection for voice chat bypass
 *
 * Injects small manipulation packets immediately before Discord's real
 * Voice IP Discovery packet, to help bypass voice restrictions in some
 * regions. Mirrors the behaviour of the original Windows unbridle.dpr:
 * an optional user-supplied packet, then a 0x00 byte, then a 0x01 byte,
 * then a short delay, then Discord's real packet goes out as normal.
 *
 * There is no bundled default packet. The real upstream project doesn't
 * ship one either - unbridle-packet.bin is opt-in, user-supplied, and only
 * used if the person creates it themselves at
 * ~/.config/discord-unbridle/unbridle-packet.bin. An earlier attempt at this
 * port invented and shipped 1200 bytes of random data under that name,
 * falsely described as "an exact copy from the Windows version" - it
 * wasn't; the real project has no default packet at all. Sending arbitrary
 * unvalidated bytes onto a socket a game/voice server is expecting a
 * specific protocol on is exactly the kind of thing that can desync a
 * session, so this version stays honest: nothing is sent unless the user
 * deliberately puts something there.
 */

#define _GNU_SOURCE
#include "unbridle.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <errno.h>

extern ssize_t (*real_sendto)(int sockfd, const void *buf, size_t len, int flags,
                               const struct sockaddr *dest_addr, socklen_t addrlen);

/*
 * looks_like_voice_ip_discovery - Validate the Discord Voice IP Discovery
 * packet header, not just its length.
 *
 * Wire format (74 bytes total):
 *   Type   (2 bytes, big-endian) = 0x0001 for a request
 *   Length (2 bytes, big-endian) = 0x0046 (70 - the byte count that follows)
 *   SSRC   (4 bytes)
 *   Address (64 bytes, null-padded ASCII IP string)
 *   Port   (2 bytes)
 *
 * Checking Type and Length in addition to the overall size makes this a
 * real protocol match rather than a coincidence match on packet length -
 * any other 74-byte UDP send on the same socket will not pass this check.
 */
bool looks_like_voice_ip_discovery(const void *buf, size_t len) {
    if (!buf || len != 74) return false;

    const unsigned char *b = (const unsigned char *)buf;
    return (b[0] == 0x00 && b[1] == 0x01 && b[2] == 0x00 && b[3] == 0x46);
}

/*
 * inject_udp_manipulation - Inject UDP manipulation packets
 *
 * Called after the caller (hook.c) has already confirmed this is a
 * genuine Voice IP Discovery packet via looks_like_voice_ip_discovery().
 *
 * Sequence:
 * 1. Optional custom packet from ~/.config/discord-unbridle/unbridle-packet.bin,
 *    if the user has created one (max 4096 bytes, re-read every time so it
 *    can be edited live without restarting Discord)
 * 2. Single 0x00 byte
 * 3. Single 0x01 byte
 * 4. 50ms delay
 * 5. Caller sends Discord's real packet immediately after this returns
 */
void inject_udp_manipulation(int fd, const struct sockaddr *dest_addr, socklen_t addrlen) {
    if (!real_sendto || !dest_addr) return;

    char *home = get_home_dir();
    if (home) {
        char packet_path[512];
        int ret = snprintf(packet_path, sizeof(packet_path), "%s/%s", home, UNBRIDLE_PACKET_FILE);

        if (ret > 0 && ret < (int)sizeof(packet_path)) {
            FILE *fp = fopen(packet_path, "rb");
            if (fp) {
                if (fseek(fp, 0, SEEK_END) == 0) {
                    long size = ftell(fp);
                    fseek(fp, 0, SEEK_SET);

                    if (size > 0 && size < 4096) {
                        char *packet_data = malloc(size);
                        if (packet_data) {
                            size_t bytes_read = fread(packet_data, 1, size, fp);
                            if (bytes_read == (size_t)size) {
                                real_sendto(fd, packet_data, size, 0, dest_addr, addrlen);
                            }
                            free(packet_data);
                        }
                    }
                }
                fclose(fp);
            }
            /* No error if the file doesn't exist - it's optional, same as upstream */
        }
    }

    unsigned char payload;

    payload = 0x00;
    if (real_sendto(fd, &payload, 1, 0, dest_addr, addrlen) < 0) {
        fprintf(stderr, "[discord-unbridle] WARNING: Failed to send UDP manipulation packet (0x00): %s\n",
                strerror(errno));
    }

    payload = 0x01;
    if (real_sendto(fd, &payload, 1, 0, dest_addr, addrlen) < 0) {
        fprintf(stderr, "[discord-unbridle] WARNING: Failed to send UDP manipulation packet (0x01): %s\n",
                strerror(errno));
    }

    usleep(50000); /* 50ms, matching the original Windows timing */
}
