/*
 * config.c - Configuration file parsing and management
 *
 * Handles reading and writing the unbridle.ini configuration file.
 * Format: INI-style with [unbridle] section and proxy URL parsing.
 *
 * Proxy URL format: [protocol://][username:password@]host:port
 * Example: http://user:pass@127.0.0.1:1080
 */

#include "unbridle.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <pwd.h>
#include <ctype.h>
#include <errno.h>

/*
 * get_home_dir - Get user's home directory path
 *
 * Returns: Home directory path, or NULL on error
 *
 * Tries $HOME environment variable first, falls back to passwd database.
 * Thread-safe on systems where getpwuid is thread-safe.
 */
char* get_home_dir(void) {
    char *home = getenv("HOME");
    if (!home) {
        struct passwd *pw = getpwuid(getuid());
        home = pw ? pw->pw_dir : NULL;
    }
    return home;
}

/*
 * get_config_path - Build full path to unbridle.ini configuration file
 *
 * Returns: Static buffer containing config path, or NULL if HOME not found
 *
 * WARNING: Returns static buffer - not thread-safe, not reentrant.
 * Path format: $HOME/.config/discord-unbridle/unbridle.ini
 */
char* get_config_path(void) {
    static char path[512];
    char *home = get_home_dir();
    if (!home) return NULL;

    int written = snprintf(path, sizeof(path), "%s/%s", home, UNBRIDLE_CONFIG_FILE);

    /* Check for truncation */
    if (written < 0 || written >= (int)sizeof(path)) {
        return NULL;
    }

    return path;
}

/*
 * trim - Remove leading and trailing whitespace from string in-place
 *
 * @str: String to trim (modified in-place)
 *
 * Handles empty strings and strings with only whitespace safely.
 */
static void trim(char *str) {
    if (!str || !*str) return;

    char *start = str;
    char *end;

    /* Trim leading spaces */
    while (isspace((unsigned char)*start)) start++;

    /* Handle all-whitespace string */
    if (*start == '\0') {
        *str = '\0';
        return;
    }

    /* Trim trailing spaces */
    end = start + strlen(start) - 1;
    while (end > start && isspace((unsigned char)*end)) end--;
    *(end + 1) = '\0';

    /* Move trimmed string to beginning if needed */
    if (start != str) {
        memmove(str, start, strlen(start) + 1);
    }
}

/*
 * load_config - Load proxy configuration from unbridle.ini file
 *
 * @config: Configuration structure to populate (output)
 *
 * Returns: true on success (even if file doesn't exist - uses defaults),
 *          false only on invalid input
 *
 * Always initializes config with safe defaults before parsing.
 * Missing file is not an error - Direct Mode is the default.
 * Supports both http:// and socks5:// protocols.
 */
bool load_config(proxy_config_t *config) {
    if (!config) return false;

    /* Initialize with safe defaults */
    memset(config, 0, sizeof(proxy_config_t));
    config->enabled = false;
    config->type = PROXY_TYPE_NONE;
    config->port = 0;
    config->has_auth = false;

    char *config_path = get_config_path();
    if (!config_path) return true; /* No config path available - use defaults */

    FILE *fp = fopen(config_path, "r");
    if (!fp) return true; /* File doesn't exist - use defaults (not an error) */

    char line[512];
    bool in_unbridle_section = false;

    while (fgets(line, sizeof(line), fp)) {
        trim(line);

        /* Skip empty lines and comments */
        if (line[0] == '\0' || line[0] == ';' || line[0] == '#') continue;

        /* Check for section header */
        if (line[0] == '[') {
            in_unbridle_section = (strncmp(line, "[unbridle]", 8) == 0);
            continue;
        }

        if (!in_unbridle_section) continue;

        /* Parse key=value pairs */
        char *equals = strchr(line, '=');
        if (!equals) continue;

        *equals = '\0';
        char *key = line;
        char *value = equals + 1;
        trim(key);
        trim(value);

        if (strcmp(key, "proxy") == 0 && strlen(value) > 0) {
            /* Parse proxy URL: [protocol://][user:pass@]host:port */
            config->enabled = true;

            /* Make a working copy to avoid modifying the original during parsing */
            char proxy_url[512];
            strncpy(proxy_url, value, sizeof(proxy_url) - 1);
            proxy_url[sizeof(proxy_url) - 1] = '\0';

            char *at = strstr(proxy_url, "://");
            char *start = proxy_url;

            /* Extract protocol */
            if (at) {
                *at = '\0';
                if (strcmp(proxy_url, "http") == 0 || strcmp(proxy_url, "https") == 0) {
                    config->type = PROXY_TYPE_HTTP;
                } else if (strcmp(proxy_url, "socks5") == 0) {
                    config->type = PROXY_TYPE_SOCKS5;
                } else {
                    /* Unknown protocol - default to HTTP */
                    config->type = PROXY_TYPE_HTTP;
                }
                start = at + 3;
            } else {
                /* No protocol specified - default to HTTP */
                config->type = PROXY_TYPE_HTTP;
            }

            /* Extract authentication credentials */
            at = strchr(start, '@');
            if (at) {
                *at = '\0';
                char *colon = strchr(start, ':');
                if (colon) {
                    *colon = '\0';
                    strncpy(config->username, start, sizeof(config->username) - 1);
                    config->username[sizeof(config->username) - 1] = '\0';

                    strncpy(config->password, colon + 1, sizeof(config->password) - 1);
                    config->password[sizeof(config->password) - 1] = '\0';

                    config->has_auth = true;
                }
                start = at + 1;
            }

            /* Extract host:port */
            char *colon = strchr(start, ':');
            if (colon) {
                *colon = '\0';
                strncpy(config->host, start, sizeof(config->host) - 1);
                config->host[sizeof(config->host) - 1] = '\0';

                int port = atoi(colon + 1);
                /* Validate port range */
                if (port > 0 && port <= 65535) {
                    config->port = port;
                } else {
                    /* Invalid port - disable this config */
                    config->enabled = false;
                }
            } else {
                /* No port specified - invalid config */
                config->enabled = false;
            }
        }
    }

    fclose(fp);
    return true;
}

/*
 * save_config - Write proxy configuration to unbridle.ini file
 *
 * @config: Configuration to save
 *
 * Returns: true on success, false on error
 *
 * Creates parent directories if they don't exist.
 * Writes empty proxy line for Direct Mode (UDP only, no proxy).
 * File permissions: 0644 (readable by user/group, writable by user only)
 */
bool save_config(const proxy_config_t *config) {
    if (!config) return false;

    char *config_path = get_config_path();
    if (!config_path) return false;

    /* Create directory if it doesn't exist */
    char dir_path[512];
    strncpy(dir_path, config_path, sizeof(dir_path) - 1);
    dir_path[sizeof(dir_path) - 1] = '\0';

    char *last_slash = strrchr(dir_path, '/');
    if (last_slash) {
        *last_slash = '\0';
        /* mkdir returns -1 if exists, which is fine */
        if (mkdir(dir_path, 0755) != 0 && errno != EEXIST) {
            return false;
        }
    }

    FILE *fp = fopen(config_path, "w");
    if (!fp) return false;

    fprintf(fp, "[unbridle]\n");

    /* Validate configuration before writing */
    if (config->enabled && config->port > 0 && config->port <= 65535 && config->host[0] != '\0') {
        fprintf(fp, "proxy = ");

        /* Write protocol */
        if (config->type == PROXY_TYPE_SOCKS5) {
            fprintf(fp, "socks5://");
        } else {
            fprintf(fp, "http://");
        }

        /* Write authentication if present */
        if (config->has_auth && config->username[0] && config->password[0]) {
            /* Note: Passwords are stored in plaintext - user should secure the file */
            fprintf(fp, "%s:%s@", config->username, config->password);
        }

        /* Write host:port */
        fprintf(fp, "%s:%d\n", config->host, config->port);
    } else {
        /* Empty proxy = Direct Mode (UDP manipulation only) */
        fprintf(fp, "proxy = \n");
    }

    fclose(fp);

    /* Set file permissions to user-only read/write (contains potential passwords) */
    chmod(config_path, 0600);

    return true;
}

/*
 * save_raw_proxy_line - Write a raw proxy URL string straight to unbridle.ini
 *
 * @raw_proxy_url: Full URL as the user typed it (e.g. "socks5://user:pass@host:1080"),
 *                 or NULL/empty for Direct Mode.
 *
 * Returns: true on success, false on error
 *
 * Used by the minimal single-field GUI, which takes one proxy URL string
 * rather than separate host/port/type/auth fields. Deliberately does not
 * re-parse or validate the URL here - load_config() already implements the
 * full "[protocol://][user:pass@]host:port" grammar and will parse this
 * same string when the hook loads, so keeping validation in one place
 * avoids the two ever drifting apart.
 */
bool save_raw_proxy_line(const char *raw_proxy_url) {
    char *config_path = get_config_path();
    if (!config_path) return false;

    if (raw_proxy_url && (strchr(raw_proxy_url, '\n') || strchr(raw_proxy_url, '\r'))) {
        return false; /* Would corrupt the ini file structure */
    }

    char dir_path[512];
    strncpy(dir_path, config_path, sizeof(dir_path) - 1);
    dir_path[sizeof(dir_path) - 1] = '\0';

    char *last_slash = strrchr(dir_path, '/');
    if (last_slash) {
        *last_slash = '\0';
        if (mkdir(dir_path, 0755) != 0 && errno != EEXIST) {
            return false;
        }
    }

    FILE *fp = fopen(config_path, "w");
    if (!fp) return false;

    fprintf(fp, "[unbridle]\n");
    if (raw_proxy_url && raw_proxy_url[0] != '\0') {
        fprintf(fp, "proxy = %s\n", raw_proxy_url);
    } else {
        fprintf(fp, "proxy = \n"); /* Direct Mode */
    }

    fclose(fp);
    chmod(config_path, 0600);
    return true;
}
