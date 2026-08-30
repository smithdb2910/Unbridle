/*
 * main.c - Discord Unbridle Linux, minimal GUI
 *
 * Small dark-themed window: a status line, one optional proxy field
 * (Direct Mode is preselected, matching the Windows installer's default),
 * and Activate / Deactivate buttons. That's the whole app.
 *
 * "Activate" does three things:
 * 1. Saves the proxy setting (or none, for Direct Mode) to
 *    ~/.config/discord-unbridle/unbridle.ini
 * 2. Writes an LD_PRELOAD-wrapped override of the REAL Discord .desktop
 *    launcher into ~/.local/share/applications, under the same filename
 *    the system one uses. XDG treats a user-level file with the same name
 *    as higher priority than the system one, so every existing shortcut,
 *    taskbar pin, and app-grid entry picks up the hook automatically -
 *    no separate icon to go find.
 * 3. Nothing is written system-wide and no other app is touched. This is
 *    strictly a per-process hook on Discord's own binary, same as the
 *    Windows version - it has no effect on the rest of your network
 *    traffic or other applications.
 *
 * "Deactivate" removes that override (restoring the system launcher, or
 * the user's own pre-existing override if there was one).
 */

#define _XOPEN_SOURCE 500
#include <gtk/gtk.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <limits.h>
#include <errno.h>
#include "unbridle.h"

typedef struct {
    GtkWidget *window;
    GtkWidget *status_label;
    GtkWidget *direct_mode_checkbox;
    GtkWidget *proxy_entry;
    GtkWidget *activate_btn;
    GtkWidget *deactivate_btn;
    GtkWidget *reset_btn;
    bool is_activated;
} AppWidgets;

/* ---------------------------------------------------------------------
 * Desktop-file shadowing: make the REAL Discord launcher use the hook
 * ------------------------------------------------------------------- */

static bool shadow_marker_path(char *out, size_t out_size) {
    char *home = get_home_dir();
    if (!home) return false;
    int ret = snprintf(out, out_size, "%s/.config/discord-unbridle/.shadowed-desktop-file", home);
    return (ret > 0 && ret < (int)out_size);
}

/*
 * find_discord_desktop_file - Locate the system .desktop entry for native
 * Discord, so we can shadow it under the exact same filename.
 */
static bool find_discord_desktop_file(char *filename_out, size_t filename_size) {
    const char *locations[] = {
        "/usr/share/applications/discord.desktop",
        "/usr/local/share/applications/discord.desktop",
        "/usr/share/applications/discord-stable.desktop",
    };
    for (size_t i = 0; i < sizeof(locations) / sizeof(locations[0]); i++) {
        if (access(locations[i], F_OK) == 0) {
            const char *base = strrchr(locations[i], '/');
            base = base ? base + 1 : locations[i];
            strncpy(filename_out, base, filename_size - 1);
            filename_out[filename_size - 1] = '\0';
            return true;
        }
    }
    return false;
}

/*
 * find_discord_path - Locate the Discord executable.
 *
 * Only native (non-sandboxed) installs work: Flatpak and Snap wrap Discord
 * in a sandbox that does not pass LD_PRELOAD through to the process inside
 * it, so the hook cannot reach it there. That's a real, verifiable
 * limitation of sandboxing, not a claim about Discord's own version
 * numbers - this hook has no dependency on which Discord version is
 * installed, native or otherwise.
 */
static bool find_discord_path(char *path, size_t path_size) {
    const char *locations[] = {
        "/usr/bin/discord",
        "/usr/local/bin/discord",
        "/opt/discord/Discord",
        "/usr/share/discord/Discord",
    };
    for (size_t i = 0; i < sizeof(locations) / sizeof(locations[0]); i++) {
        if (access(locations[i], X_OK) == 0) {
            strncpy(path, locations[i], path_size - 1);
            path[path_size - 1] = '\0';
            return true;
        }
    }

    /* Fedora has no official Discord package - most people extract the
     * official .tar.gz into their home directory instead. */
    char *home = get_home_dir();
    if (home) {
        const char *home_relative[] = {
            "Discord/Discord",
            ".local/share/Discord/Discord",
            "Downloads/Discord/Discord",
        };
        char candidate[512];
        for (size_t i = 0; i < sizeof(home_relative) / sizeof(home_relative[0]); i++) {
            int ret = snprintf(candidate, sizeof(candidate), "%s/%s", home, home_relative[i]);
            if (ret > 0 && ret < (int)sizeof(candidate) && access(candidate, X_OK) == 0) {
                strncpy(path, candidate, path_size - 1);
                path[path_size - 1] = '\0';
                return true;
            }
        }
    }

    return false;
}

static bool find_hook_library(char *lib_path, size_t lib_path_size) {
    char exe_path[512];
    ssize_t len = readlink("/proc/self/exe", exe_path, sizeof(exe_path) - 1);
    if (len == -1) return false;
    exe_path[len] = '\0';

    char *last_slash = strrchr(exe_path, '/');
    if (!last_slash) return false;
    *last_slash = '\0';

    int ret = snprintf(lib_path, lib_path_size, "%s/libdiscord-unbridle.so", exe_path);
    if (ret < 0 || ret >= (int)lib_path_size) return false;

    if (access(lib_path, F_OK) == 0) return true;

    strncpy(lib_path, "/usr/lib/discord-unbridle/libdiscord-unbridle.so", lib_path_size - 1);
    lib_path[lib_path_size - 1] = '\0';
    return (access(lib_path, F_OK) == 0);
}

/*
 * write_shadowed_desktop_file - Create/overwrite the user-level override.
 * Backs up a pre-existing non-Unbridle override before replacing it.
 */
static bool write_shadowed_desktop_file(const char *filename, const char *discord_path, const char *lib_path) {
    char *home = get_home_dir();
    if (!home) return false;

    char apps_dir[512];
    int ret = snprintf(apps_dir, sizeof(apps_dir), "%s/.local/share/applications", home);
    if (ret < 0 || ret >= (int)sizeof(apps_dir)) return false;
    if (mkdir(apps_dir, 0755) != 0 && errno != EEXIST) return false;

    char target_path[600];
    ret = snprintf(target_path, sizeof(target_path), "%s/%s", apps_dir, filename);
    if (ret < 0 || ret >= (int)sizeof(target_path)) return false;

    if (access(target_path, F_OK) == 0) {
        bool ours = false;
        FILE *check = fopen(target_path, "r");
        if (check) {
            char line[512];
            while (fgets(line, sizeof(line), check)) {
                if (strstr(line, "X-Unbridle-Generated=true")) { ours = true; break; }
            }
            fclose(check);
        }
        if (!ours) {
            char backup_path[650];
            snprintf(backup_path, sizeof(backup_path), "%s.pre-unbridle-backup", target_path);
            if (access(backup_path, F_OK) != 0) {
                rename(target_path, backup_path);
            }
        }
    }

    FILE *fp = fopen(target_path, "w");
    if (!fp) return false;

    fprintf(fp, "[Desktop Entry]\n");
    fprintf(fp, "Name=Discord\n");
    fprintf(fp, "Comment=Discord (proxied via Unbridle)\n");
    fprintf(fp, "Exec=env LD_PRELOAD=%s %s\n", lib_path, discord_path);
    fprintf(fp, "Icon=discord\n");
    fprintf(fp, "Type=Application\n");
    fprintf(fp, "Categories=Network;InstantMessaging;\n");
    fprintf(fp, "StartupWMClass=discord\n");
    fprintf(fp, "X-Unbridle-Generated=true\n");

    fclose(fp);
    chmod(target_path, 0755);

    char marker[512];
    if (shadow_marker_path(marker, sizeof(marker))) {
        FILE *mf = fopen(marker, "w");
        if (mf) {
            fprintf(mf, "%s\n", filename);
            fclose(mf);
        }
    }

    return true;
}

static void remove_shadowed_desktop_file(void) {
    char marker[512];
    if (!shadow_marker_path(marker, sizeof(marker))) return;

    FILE *mf = fopen(marker, "r");
    if (!mf) return;

    char filename[256];
    bool have_line = (fgets(filename, sizeof(filename), mf) != NULL);
    fclose(mf);
    if (!have_line) return;

    size_t flen = strlen(filename);
    if (flen > 0 && filename[flen - 1] == '\n') filename[flen - 1] = '\0';

    char *home = get_home_dir();
    if (!home || filename[0] == '\0') { unlink(marker); return; }

    char target_path[650];
    snprintf(target_path, sizeof(target_path), "%s/.local/share/applications/%s", home, filename);

    char backup_path[700];
    snprintf(backup_path, sizeof(backup_path), "%s.pre-unbridle-backup", target_path);

    if (access(backup_path, F_OK) == 0) {
        rename(backup_path, target_path);
    } else {
        unlink(target_path);
    }

    unlink(marker);
}

static bool check_activation_status(void) {
    char marker[512];
    if (!shadow_marker_path(marker, sizeof(marker))) return false;
    return (access(marker, F_OK) == 0);
}

static bool is_discord_running(void) {
    int result = system("pgrep -x discord >/dev/null 2>&1 || "
                       "pgrep -x Discord >/dev/null 2>&1 || "
                       "pgrep -x discord-canary >/dev/null 2>&1 || "
                       "pgrep -x discord-ptb >/dev/null 2>&1");
    return (result == 0);
}

/* ---------------------------------------------------------------------
 * UI
 * ------------------------------------------------------------------- */

static void apply_dark_theme(void) {
    GtkCssProvider *provider = gtk_css_provider_new();
    const char *css =
        "window { background-color: #2b2d31; }"
        "label { color: #dbdee1; }"
        "#status-active { color: #23a55a; font-weight: bold; }"
        "#status-inactive { color: #949ba4; font-weight: bold; }"
        "entry { background-color: #1e1f22; color: #dbdee1; border: 1px solid #1e1f22; border-radius: 4px; }"
        "checkbutton { color: #dbdee1; }"
        "button { border-radius: 4px; min-height: 32px; }"
        "#activate-btn { background-image: none; background-color: #5865f2; color: white; }"
        "#activate-btn:hover { background-color: #4752c4; }"
        "#deactivate-btn { background-image: none; background-color: #4e5058; color: white; }"
        "#deactivate-btn:hover { background-color: #6d6f78; }"
        "#reset-btn { background-image: none; background-color: #ed4245; color: white; }"
        "#reset-btn:hover { background-color: #c73537; }"
        "#activate-btn:disabled, #deactivate-btn:disabled, #reset-btn:disabled { background-color: #35363c; color: #6d6f78; }";

    gtk_css_provider_load_from_data(provider, css, -1, NULL);
    gtk_style_context_add_provider_for_screen(
        gdk_screen_get_default(),
        GTK_STYLE_PROVIDER(provider),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION
    );
    g_object_unref(provider);
}

static void update_ui_state(AppWidgets *w) {
    w->is_activated = check_activation_status();

    gtk_widget_set_sensitive(w->direct_mode_checkbox, !w->is_activated);
    gtk_widget_set_sensitive(w->proxy_entry,
        !w->is_activated && !gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(w->direct_mode_checkbox)));
    gtk_widget_set_sensitive(w->activate_btn, !w->is_activated);
    gtk_widget_set_sensitive(w->deactivate_btn, w->is_activated);
    gtk_widget_set_sensitive(w->reset_btn, TRUE);

    gtk_widget_set_name(w->status_label, w->is_activated ? "status-active" : "status-inactive");

    gtk_label_set_text(GTK_LABEL(w->status_label),
        w->is_activated ? "\u25cf Activated" : "\u25cb Deactivated");
}

static void on_direct_mode_toggled(GtkToggleButton *toggle, AppWidgets *w) {
    gboolean direct = gtk_toggle_button_get_active(toggle);
    gtk_widget_set_sensitive(w->proxy_entry, !direct);
}

static void show_dialog(AppWidgets *w, GtkMessageType type, const char *title, const char *body) {
    GtkWidget *dialog = gtk_message_dialog_new(GTK_WINDOW(w->window), GTK_DIALOG_MODAL,
                                                type, GTK_BUTTONS_OK, "%s", title);
    if (body) {
        gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(dialog), "%s", body);
    }
    gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);
}

static void on_activate_clicked(GtkButton *button, AppWidgets *w) {
    (void)button;

    if (is_discord_running()) {
        show_dialog(w, GTK_MESSAGE_WARNING, "Discord is running",
            "Close Discord first, then activate - the hook only takes effect on the next launch.");
        return;
    }

    char discord_path[512];
    if (!find_discord_path(discord_path, sizeof(discord_path))) {
        show_dialog(w, GTK_MESSAGE_ERROR, "Discord not found",
            "Couldn't find a native Discord install. Flatpak and Snap builds are sandboxed and "
            "can't be hooked this way - install Discord's official native package or .tar.gz instead.");
        return;
    }

    char lib_path[512];
    if (!find_hook_library(lib_path, sizeof(lib_path))) {
        show_dialog(w, GTK_MESSAGE_ERROR, "Hook library missing",
            "libdiscord-unbridle.so wasn't found next to this program. Rebuild with 'make' first.");
        return;
    }

    gboolean direct_mode = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(w->direct_mode_checkbox));
    const char *proxy_text = direct_mode ? "" : gtk_entry_get_text(GTK_ENTRY(w->proxy_entry));

    if (!save_raw_proxy_line(proxy_text)) {
        show_dialog(w, GTK_MESSAGE_ERROR, "Couldn't save configuration", NULL);
        return;
    }

    char shadow_filename[256];
    if (!find_discord_desktop_file(shadow_filename, sizeof(shadow_filename))) {
        strncpy(shadow_filename, "discord.desktop", sizeof(shadow_filename) - 1);
        shadow_filename[sizeof(shadow_filename) - 1] = '\0';
    }

    if (!write_shadowed_desktop_file(shadow_filename, discord_path, lib_path)) {
        show_dialog(w, GTK_MESSAGE_ERROR, "Activation failed",
            "Couldn't write the launcher override to ~/.local/share/applications.");
        return;
    }

    show_dialog(w, GTK_MESSAGE_INFO, "Activated",
        "Launch Discord the way you normally do - it's patched to use this automatically.");
    update_ui_state(w);
}

static void on_deactivate_clicked(GtkButton *button, AppWidgets *w) {
    (void)button;
    remove_shadowed_desktop_file();
    show_dialog(w, GTK_MESSAGE_INFO, "Deactivated", "Discord will launch normally again.");
    update_ui_state(w);
}

static void on_reset_clicked(GtkButton *button, AppWidgets *w) {
    (void)button;

    remove_shadowed_desktop_file();
    save_raw_proxy_line("");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(w->direct_mode_checkbox), TRUE);
    gtk_entry_set_text(GTK_ENTRY(w->proxy_entry), "");

    show_dialog(w, GTK_MESSAGE_INFO, "Reset complete",
        "Unbridle has been reset to Direct Mode. Restart Discord if it is currently running.");
    update_ui_state(w);
}

static GtkWidget *build_window(AppWidgets *w) {
    w->window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(w->window), "Discord Unbridle");
    gtk_window_set_default_size(GTK_WINDOW(w->window), 300, 190);
    gtk_window_set_resizable(GTK_WINDOW(w->window), FALSE);
    gtk_container_set_border_width(GTK_CONTAINER(w->window), 16);
    g_signal_connect(w->window, "destroy", G_CALLBACK(gtk_main_quit), NULL);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_container_add(GTK_CONTAINER(w->window), vbox);

    w->status_label = gtk_label_new("");
    gtk_widget_set_halign(w->status_label, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(vbox), w->status_label, FALSE, FALSE, 0);

    w->direct_mode_checkbox = gtk_check_button_new_with_label("Direct Mode (recommended)");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(w->direct_mode_checkbox), TRUE);
    g_signal_connect(w->direct_mode_checkbox, "toggled", G_CALLBACK(on_direct_mode_toggled), w);
    gtk_box_pack_start(GTK_BOX(vbox), w->direct_mode_checkbox, FALSE, FALSE, 0);

    w->proxy_entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(w->proxy_entry), "proxy URL, e.g. socks5://host:1080");
    gtk_widget_set_sensitive(w->proxy_entry, FALSE);
    gtk_box_pack_start(GTK_BOX(vbox), w->proxy_entry, FALSE, FALSE, 0);

    GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_pack_start(GTK_BOX(vbox), hbox, FALSE, FALSE, 4);

    w->activate_btn = gtk_button_new_with_label("Activate");
    gtk_widget_set_name(w->activate_btn, "activate-btn");
    g_signal_connect(w->activate_btn, "clicked", G_CALLBACK(on_activate_clicked), w);
    gtk_box_pack_start(GTK_BOX(hbox), w->activate_btn, TRUE, TRUE, 0);

    w->deactivate_btn = gtk_button_new_with_label("Deactivate");
    gtk_widget_set_name(w->deactivate_btn, "deactivate-btn");
    g_signal_connect(w->deactivate_btn, "clicked", G_CALLBACK(on_deactivate_clicked), w);
    gtk_box_pack_start(GTK_BOX(hbox), w->deactivate_btn, TRUE, TRUE, 0);

    w->reset_btn = gtk_button_new_with_label("Reset");
    gtk_widget_set_name(w->reset_btn, "reset-btn");
    g_signal_connect(w->reset_btn, "clicked", G_CALLBACK(on_reset_clicked), w);
    gtk_box_pack_start(GTK_BOX(vbox), w->reset_btn, FALSE, FALSE, 0);

    return w->window;
}

int main(int argc, char *argv[]) {
    gtk_init(&argc, &argv);
    apply_dark_theme();

    AppWidgets widgets = {0};
    build_window(&widgets);
    update_ui_state(&widgets);

    gtk_widget_show_all(widgets.window);
    gtk_main();

    return 0;
}
