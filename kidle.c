#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/socket.h>
#include <stdarg.h>
#include <fcntl.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <linux/input.h>
#include <glib.h>
#include <gio/gio.h>

#define IDLE_TIMEOUT_DEFAULT 900
#define POLL_INTERVAL_DEFAULT 5
#define MAX_INPUT_DEVS 64

static guint idle_timeout = IDLE_TIMEOUT_DEFAULT;
static guint lock_delay = 60;
static gboolean debug_mode = FALSE;
static gboolean screen_off = FALSE;
static gboolean locked = FALSE;
static guint lock_timer_id = 0;
static GMainLoop *loop = NULL;
static GDBusProxy *screensaver_proxy = NULL;
static gchar *active_bus_address = NULL;
static gchar *active_wayland = NULL;
static gchar *active_runtime = NULL;
static gint64 last_activity_time = 0;
static int input_fds[MAX_INPUT_DEVS];
static int num_input_fds = 0;
static GIOChannel *io_channels[MAX_INPUT_DEVS];
static guint io_watches[MAX_INPUT_DEVS];
static guint retry_source = 0;
static GDBusProxy *logind_manager_proxy = NULL;
static gint inhibit_fd = -1;

static void log_debug(const char *fmt, ...) G_GNUC_UNUSED;
static void log_debug(const char *fmt, ...)
{
	if (!debug_mode)
		return;
	va_list ap;
	va_start(ap, fmt);
	g_logv("kidle", G_LOG_LEVEL_DEBUG, fmt, ap);
	va_end(ap);
}

static void log_info(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	g_logv("kidle", G_LOG_LEVEL_INFO, fmt, ap);
	va_end(ap);
}

static gboolean lock_screen(void);

static gboolean lock_delay_cb(G_GNUC_UNUSED gpointer data)
{
	log_info("kidle: lock delay expired, locking screen");
	lock_screen();
	lock_timer_id = 0;
	return G_SOURCE_REMOVE;
}

static void update_activity(void)
{
	last_activity_time = g_get_monotonic_time() / 1000;
	if (screen_off || locked) {
		log_info("kidle: user activity detected, resetting state");
		locked = FALSE;
		screen_off = FALSE;
	}
	if (lock_timer_id) {
		g_source_remove(lock_timer_id);
		lock_timer_id = 0;
		log_info("kidle: lock delay cancelled by activity");
	}
}

static gboolean run_kscreen_doctor(const gchar *mode)
{
	gint wait_status = 0;
	GError *error = NULL;
	gchar *argv[] = { "kscreen-doctor", "--dpms", (gchar *)mode, NULL };

	gchar **envp = g_get_environ();
	if (active_bus_address)
		envp = g_environ_setenv(envp, "DBUS_SESSION_BUS_ADDRESS", active_bus_address, TRUE);
	if (active_wayland)
		envp = g_environ_setenv(envp, "WAYLAND_DISPLAY", active_wayland, TRUE);
	if (active_runtime)
		envp = g_environ_setenv(envp, "XDG_RUNTIME_DIR", active_runtime, TRUE);
	if (active_wayland)
		envp = g_environ_setenv(envp, "QT_QPA_PLATFORM", "wayland", TRUE);

	gboolean ok = g_spawn_sync(NULL, argv, envp, G_SPAWN_SEARCH_PATH,
		NULL, NULL, NULL, NULL, &wait_status, &error);
	if (!ok) {
		log_info("kidle: failed to run kscreen-doctor: %s", error->message);
		g_error_free(error);
		g_strfreev(envp);
		return FALSE;
	}
	g_strfreev(envp);

	if (!g_spawn_check_wait_status(wait_status, &error)) {
		log_info("kidle: kscreen-doctor failed: %s", error->message);
		g_error_free(error);
		return FALSE;
	}

	return TRUE;
}

static gboolean dpms_off(void)
{
	log_info("kidle: turning screens off via DPMS");
	return run_kscreen_doctor("off");
}

static gboolean dpms_on(void)
{
	log_info("kidle: turning screens on via DPMS");
	return run_kscreen_doctor("on");
}

static gboolean lock_screen(void);

static void inhibit_suspend(void)
{
	if (inhibit_fd >= 0)
		return;
	if (!logind_manager_proxy)
		return;

	GError *error = NULL;
	GVariant *result = g_dbus_proxy_call_sync(logind_manager_proxy,
		"Inhibit",
		g_variant_new("(ssss)",
			"sleep",
			"kidle",
			"Desktop idle management - preventing suspend",
			"block"),
		G_DBUS_CALL_FLAGS_NONE,
		-1, NULL, &error);

	if (!result) {
		log_info("kidle: failed to inhibit suspend: %s", error->message);
		g_error_free(error);
		return;
	}

	/* Inhibit returns a file descriptor handle directly */
	gint32 fd = g_variant_get_handle(result);
	if (fd >= 0) {
		inhibit_fd = dup(fd);
		log_info("kidle: inhibited system sleep/suspend");
	}
	g_variant_unref(result);
}

static void uninhibit_suspend(void)
{
	if (inhibit_fd >= 0) {
		close(inhibit_fd);
		inhibit_fd = -1;
		log_info("kidle: released suspend inhibition");
	}
}

static gboolean lock_screen(void)
{
	if (screensaver_proxy) {
		GError *error = NULL;
		GVariant *result = g_dbus_proxy_call_sync(screensaver_proxy,
			"Lock", NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, &error);
		if (!result) {
			log_info("kidle: failed to lock screen via KWin: %s", error->message);
			g_error_free(error);
		} else {
			g_variant_unref(result);
			return TRUE;
		}
	}

	gint wait_status = 0;
	GError *error = NULL;
	char *argv[] = { "loginctl", "lock-sessions", NULL };

	if (!g_spawn_sync(NULL, argv, NULL, G_SPAWN_SEARCH_PATH,
		NULL, NULL, NULL, NULL, &wait_status, &error)) {
		log_info("kidle: failed to run loginctl: %s", error->message);
		g_error_free(error);
		return FALSE;
	}

	if (!g_spawn_check_wait_status(wait_status, &error)) {
		log_info("kidle: loginctl failed: %s", error->message);
		g_error_free(error);
		return FALSE;
	}

	log_info("kidle: locked via loginctl");
	return TRUE;
}

static gboolean do_lock_and_off(void)
{
	inhibit_suspend();

	if (!screen_off) {
		if (dpms_off())
			screen_off = TRUE;
	}

	if (!locked && !lock_timer_id) {
		if (lock_delay > 0) {
			log_info("kidle: screens off, lock in %ds", lock_delay);
			lock_timer_id = g_timeout_add_seconds(lock_delay, lock_delay_cb, NULL);
			locked = FALSE;
		} else {
			log_info("kidle: locking screen immediately");
			lock_screen();
			locked = TRUE;
		}
	}

	return TRUE;
}

static void connect_screensaver(void);

static gboolean on_idle_check(G_GNUC_UNUSED gpointer user_data)
{
	if (screensaver_proxy) {
		gboolean is_active = FALSE;
		GVariant *result = g_dbus_proxy_call_sync(screensaver_proxy,
			"GetActive", NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL);
		if (result) {
			g_variant_get(result, "(b)", &is_active);
			g_variant_unref(result);
		}

		if (is_active) {
			if (!screen_off) {
				log_info("kidle: screensaver active, forcing DPMS off");
				if (dpms_off())
					screen_off = TRUE;
			}
			if (!locked && !lock_timer_id) {
				if (lock_delay > 0) {
					log_info("kidle: screensaver active, lock in %ds", lock_delay);
					lock_timer_id = g_timeout_add_seconds(lock_delay, lock_delay_cb, NULL);
				} else {
					log_info("kidle: screensaver active, locking immediately");
					lock_screen();
					locked = TRUE;
				}
			}
			return G_SOURCE_CONTINUE;
		}
	}

	gint64 now = g_get_monotonic_time() / 1000;
	gint64 idle_ms = now - last_activity_time;

	if (idle_ms >= (gint64)idle_timeout * 1000 && !locked && !lock_timer_id) {
		log_info("kidle: idle for %llds (timeout %ds), locking and turning off",
			(long long)(idle_ms / 1000), idle_timeout);
		do_lock_and_off();
	}

	return G_SOURCE_CONTINUE;
}

static void on_screensaver_active_changed(G_GNUC_UNUSED GDBusProxy *proxy,
	GVariant *changed, G_GNUC_UNUSED GStrv invalidated,
	G_GNUC_UNUSED gpointer user_data)
{
	if (!changed)
		return;

	GVariantDict dict;
	g_variant_dict_init(&dict, changed);

	gboolean active = FALSE;
	if (g_variant_dict_lookup(&dict, "Active", "b", &active)) {
		if (active) {
			log_info("kidle: screensaver activated, forcing DPMS off");
			if (!screen_off) {
				if (dpms_off())
					screen_off = TRUE;
			}
			if (!locked && !lock_timer_id) {
				if (lock_delay > 0) {
					log_info("kidle: screensaver active, lock in %ds", lock_delay);
					lock_timer_id = g_timeout_add_seconds(lock_delay, lock_delay_cb, NULL);
				} else {
					log_info("kidle: screensaver active, locking immediately");
					lock_screen();
					locked = TRUE;
				}
			}
		} else {
			log_info("kidle: screensaver deactivated, letting PowerDevil restore screens");
			locked = FALSE;
			screen_off = FALSE;
			if (lock_timer_id) {
				g_source_remove(lock_timer_id);
				lock_timer_id = 0;
				log_info("kidle: lock delay cancelled");
			}
			if (!screensaver_proxy)
				dpms_on();
			update_activity();
		}
	}
	g_variant_dict_clear(&dict);
}

static gboolean on_input_readable(GIOChannel *source,
	G_GNUC_UNUSED GIOCondition condition, G_GNUC_UNUSED gpointer data)
{
	struct input_event ev;
	int fd = g_io_channel_unix_get_fd(source);

	while (read(fd, &ev, sizeof(ev)) == sizeof(ev)) {
		if (ev.type == EV_KEY || ev.type == EV_REL || ev.type == EV_ABS) {
			update_activity();
			if (screen_off && !screensaver_proxy)
				dpms_on();
			break;
		}
	}

	return G_SOURCE_CONTINUE;
}

static void open_input_devices(void)
{
	DIR *dir = opendir("/dev/input");
	if (!dir) {
		log_info("kidle: cannot open /dev/input: %s", strerror(errno));
		return;
	}

	struct dirent *ent;
	while ((ent = readdir(dir)) != NULL && num_input_fds < MAX_INPUT_DEVS) {
		if (strncmp(ent->d_name, "event", 5) != 0)
			continue;

		char path[PATH_MAX];
		snprintf(path, sizeof(path), "/dev/input/%s", ent->d_name);

		int fd = open(path, O_RDONLY | O_NONBLOCK);
		if (fd < 0)
			continue;

		unsigned long evbits = 0;
		if (ioctl(fd, EVIOCGBIT(0, sizeof(evbits)), &evbits) < 0) {
			close(fd);
			continue;
		}

		gboolean has_key = (evbits & (1UL << EV_KEY)) != 0;
		gboolean has_rel = (evbits & (1UL << EV_REL)) != 0;
		gboolean has_abs = (evbits & (1UL << EV_ABS)) != 0;

		if (!(has_key || has_rel || has_abs)) {
			close(fd);
			continue;
		}

		if (has_key && !has_rel && !has_abs) {
			unsigned long keybits[4] = {0};
			ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keybits)), keybits);
			int word = BTN_LEFT / (8 * (int)sizeof(unsigned long));
			int bit = BTN_LEFT % (8 * (int)sizeof(unsigned long));
			gboolean has_mouse_btn = (word < 4) && (keybits[word] & (1UL << bit));
			gboolean has_kbd_key = (keybits[0] & ((1UL << KEY_ENTER) | (1UL << KEY_SPACE) | (1UL << KEY_A))) != 0;
			if (!has_mouse_btn && !has_kbd_key) {
				close(fd);
				continue;
			}
		}

		input_fds[num_input_fds] = fd;

		GIOChannel *ch = g_io_channel_unix_new(fd);
		g_io_channel_set_encoding(ch, NULL, NULL);
		g_io_channel_set_buffered(ch, FALSE);
		io_channels[num_input_fds] = ch;
		io_watches[num_input_fds] = g_io_add_watch(ch,
			G_IO_IN, on_input_readable, NULL);

		num_input_fds++;
		log_debug("kidle: monitoring %s", path);
	}

	closedir(dir);
	log_info("kidle: monitoring %d input devices", num_input_fds);
}

static void close_input_devices(void)
{
	for (int i = 0; i < num_input_fds; i++) {
		g_source_remove(io_watches[i]);
		g_io_channel_shutdown(io_channels[i], FALSE, NULL);
		g_io_channel_unref(io_channels[i]);
		close(input_fds[i]);
	}
	num_input_fds = 0;
}

static gboolean find_active_session_bus(void)
{
	GError *error = NULL;
	GDBusProxy *proxy = g_dbus_proxy_new_for_bus_sync(G_BUS_TYPE_SYSTEM,
		G_DBUS_PROXY_FLAGS_NONE, NULL,
		"org.freedesktop.login1",
		"/org/freedesktop/login1",
		"org.freedesktop.login1.Manager", NULL, &error);
	if (!proxy) {
		log_info("kidle: failed to connect to logind: %s", error->message);
		g_error_free(error);
		return FALSE;
	}
	g_object_unref(proxy);

	g_free(active_bus_address);
	g_free(active_wayland);
	g_free(active_runtime);

	const gchar *runtime_dir = NULL;
	GDir *dir = g_dir_open("/run/user", 0, NULL);
	if (!dir) {
		log_info("kidle: cannot open /run/user");
		return FALSE;
	}

	guint32 G_GNUC_UNUSED best_uid = 0;
	gboolean found_greeter = FALSE;
	gboolean found_user = FALSE;

	while ((runtime_dir = g_dir_read_name(dir)) != NULL) {
		gchar *endp = NULL;
		guint64 uid = g_ascii_strtoull(runtime_dir, &endp, 10);
		if (*endp != '\0')
			continue;

		gchar *bus_path = g_strdup_printf("/run/user/%s/bus", runtime_dir);
		gchar *runtime_path = g_strdup_printf("/run/user/%s", runtime_dir);

		if (!g_file_test(bus_path, G_FILE_TEST_EXISTS)) {
			g_free(bus_path);
			g_free(runtime_path);
			continue;
		}

	gboolean is_greeter = (uid != 0 && uid < 1000);
		gboolean is_regular = (uid >= 1000);

		if (is_greeter && !found_greeter) {
			active_bus_address = g_strdup_printf("unix:path=%s", bus_path);
			active_runtime = g_strdup(runtime_path);
			active_wayland = g_strdup("wayland-0");
			best_uid = (guint32)uid;
			found_greeter = TRUE;
			log_info("kidle: found greeter session uid=%s", runtime_dir);
		} else if (is_regular && !found_user) {
			if (!found_greeter) {
				active_bus_address = g_strdup_printf("unix:path=%s", bus_path);
				active_runtime = g_strdup(runtime_path);
				active_wayland = g_strdup("wayland-0");
				best_uid = (guint32)uid;
			}
			found_user = TRUE;
			log_info("kidle: found user session uid=%s", runtime_dir);
		}

		g_free(bus_path);
		g_free(runtime_path);
	}
	g_dir_close(dir);

	if (active_bus_address) {
		log_info("kidle: using session bus=%s", active_bus_address);
		return TRUE;
	}

	if (active_runtime) {
		log_info("kidle: no session bus, using runtime=%s wayland=%s",
			active_runtime, active_wayland ? active_wayland : "(null)");
		return TRUE;
	}

	log_info("kidle: no active session found");
	return FALSE;
}

static gboolean retry_detect_session(G_GNUC_UNUSED gpointer data)
{
	log_info("kidle: retrying session detection...");
	find_active_session_bus();
	connect_screensaver();

	if (active_bus_address) {
		log_info("kidle: session detected, stopping retry");
		if (retry_source) {
			g_source_remove(retry_source);
			retry_source = 0;
		}
		return G_SOURCE_REMOVE;
	}

	return G_SOURCE_CONTINUE;
}

static void connect_screensaver(void)
{
	if (!active_bus_address)
		return;

	if (geteuid() != 0) {
		GError *error = NULL;
		GDBusConnection *conn = g_dbus_connection_new_for_address_sync(
			active_bus_address,
			G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT |
			G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION,
			NULL, NULL, &error);
		if (!conn) {
			log_info("kidle: failed to connect to session bus: %s", error->message);
			g_error_free(error);
			return;
		}

		if (screensaver_proxy)
			g_object_unref(screensaver_proxy);

		screensaver_proxy = g_dbus_proxy_new_sync(conn,
			G_DBUS_PROXY_FLAGS_NONE, NULL,
			"org.kde.KWin", "/ScreenSaver",
			"org.freedesktop.ScreenSaver", NULL, &error);
		if (!screensaver_proxy) {
			log_info("kidle: failed to connect to KWin ScreenSaver: %s", error->message);
			g_error_free(error);
			g_object_unref(conn);
			return;
		}

		g_signal_connect(screensaver_proxy, "g-properties-changed",
			G_CALLBACK(on_screensaver_active_changed), NULL);
		log_info("kidle: connected to KWin ScreenSaver on %s", active_bus_address);
		g_object_unref(conn);
	} else {
		log_info("kidle: running as root, skipping KWin ScreenSaver connection");
	}
}

static void on_login_session_changed(G_GNUC_UNUSED GDBusProxy *proxy,
	G_GNUC_UNUSED GVariant *changed, G_GNUC_UNUSED GStrv invalidated,
	G_GNUC_UNUSED gpointer user_data)
{
	log_info("kidle: logind session changed, rediscovering active session");
	screen_off = FALSE;
	locked = FALSE;

	if (screensaver_proxy) {
		g_object_unref(screensaver_proxy);
		screensaver_proxy = NULL;
	}

	find_active_session_bus();
	connect_screensaver();
}

static gboolean setup_dbus(void)
{
	find_active_session_bus();
	connect_screensaver();

	GError *error = NULL;
	logind_manager_proxy = g_dbus_proxy_new_for_bus_sync(G_BUS_TYPE_SYSTEM,
		G_DBUS_PROXY_FLAGS_NONE, NULL,
		"org.freedesktop.login1",
		"/org/freedesktop/login1",
		"org.freedesktop.login1.Manager", NULL, &error);
	if (logind_manager_proxy) {
		g_signal_connect(logind_manager_proxy, "g-properties-changed",
			G_CALLBACK(on_login_session_changed), NULL);
		log_info("kidle: monitoring logind for session changes");
	} else {
		log_info("kidle: failed to connect to logind: %s", error->message);
		g_error_free(error);
	}

	/* Inhibit suspend immediately on startup since we're managing idle */
	inhibit_suspend();

	if (!active_bus_address) {
		log_info("kidle: no session found yet, will retry every 10s");
		retry_source = g_timeout_add_seconds(10, retry_detect_session, NULL);
	}

	return TRUE;
}

static void cleanup(void)
{
	uninhibit_suspend();
	close_input_devices();
	if (screensaver_proxy)
		g_object_unref(screensaver_proxy);
	if (logind_manager_proxy)
		g_object_unref(logind_manager_proxy);
	if (loop)
		g_main_loop_unref(loop);
	g_free(active_bus_address);
	g_free(active_wayland);
	g_free(active_runtime);
}

static void sig_handler(int sig)
{
	log_info("kidle: caught signal %d, exiting", sig);
	g_main_loop_quit(loop);
}

static GOptionEntry cli_entries[] = {
	{ "timeout", 't', 0, G_OPTION_ARG_INT, &idle_timeout,
		"Idle timeout in seconds (default: 300)", "SECS" },
	{ "lock-delay", 'l', 0, G_OPTION_ARG_INT, &lock_delay,
		"Delay before locking after screens off in seconds (default: 60, 0=immediate)", "SECS" },
	{ "debug", 'd', 0, G_OPTION_ARG_NONE, &debug_mode,
		"Enable debug output", NULL },
	{ NULL }
};

int main(int argc, char **argv)
{
	GError *error = NULL;
	GOptionContext *context;

	context = g_option_context_new("- KDE Plasma idle screen lock & DPMS daemon");
	g_option_context_add_main_entries(context, cli_entries, NULL);
	if (!g_option_context_parse(context, &argc, &argv, &error)) {
		fprintf(stderr, "kidle: %s\n", error->message);
		g_error_free(error);
		g_option_context_free(context);
		return 1;
	}
	g_option_context_free(context);

	if (idle_timeout < 10) {
		fprintf(stderr, "kidle: timeout too low (min 10s), using 10\n");
		idle_timeout = 10;
	}

	log_info("kidle: starting with timeout=%ds lock-delay=%ds", idle_timeout, lock_delay);

	if (!setup_dbus()) {
		fprintf(stderr, "kidle: failed to initialize\n");
		return 1;
	}

	update_activity();
	open_input_devices();

	if (num_input_fds == 0)
		fprintf(stderr, "kidle: WARNING: no input devices opened\n");

	struct sigaction sa;
	sa.sa_handler = sig_handler;
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = 0;
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGHUP, &sa, NULL);

	loop = g_main_loop_new(NULL, FALSE);

	g_timeout_add_seconds(POLL_INTERVAL_DEFAULT, on_idle_check, NULL);

	log_info("kidle: running");
	g_main_loop_run(loop);

	log_info("kidle: shutting down");
	cleanup();
	return 0;
}