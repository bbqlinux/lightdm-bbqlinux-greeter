
#include <cairo-xlib.h>
#include <gtk/gtk.h>
#include <gdk/gdkx.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <string.h>
#include <X11/Xatom.h>

#include "greeterbackground.h"

typedef enum
{
    /* Broken/uninitialized configuration */
    BACKGROUND_TYPE_INVALID,
    /* Do not use this monitor */
    BACKGROUND_TYPE_SKIP,
    /* Solid color */
    BACKGROUND_TYPE_COLOR,
    /* Path to image and scaling mode */
    BACKGROUND_TYPE_IMAGE
    /* Maybe other types (e.g. gradient) */
} BackgroundType;

static const gchar* BACKGROUND_TYPE_SKIP_VALUE = "#skip";

typedef enum
{
    /* It is not really useful, used for debugging */
    SCALING_MODE_SOURCE,
    /* Default mode for values without mode prefix */
    SCALING_MODE_ZOOMED,
    SCALING_MODE_STRETCHED
} ScalingMode;

static const gchar* SCALING_MODE_PREFIXES[] = {"#source:", "#zoomed:", "#stretched:", NULL};

/* Background configuration (parsed from background=... option).
   Used to fill <Background> */
typedef struct
{
    BackgroundType type;
    union
    {
        GdkRGBA color;
        struct
        {
            gchar *path;
            ScalingMode mode;
        } image;
    } options;
} BackgroundConfig;

/* Store monitor configuration */
typedef struct
{
    BackgroundConfig bg;
    gboolean user_bg;
    gboolean laptop;
} MonitorConfig;

/* Actual drawing information attached to monitor.
 * Used to separate configured monitor background and user background. */
typedef struct
{
    BackgroundType type;
    union
    {
        GdkPixbuf* image;
        GdkRGBA color;
    } options;
} Background;

typedef struct
{
    GreeterBackground* object;
    gint number;
    gchar* name;
    GdkRectangle geometry;
    GtkWindow* window;
    gulong window_draw_handler_id;

    /* Configured background */
    Background background_configured;
    /* Background used to display user-background */
    Background background_custom;
    /* Current monitor background: &background_configured or &background_custom
     * Monitors with type = BACKGROUND_TYPE_SKIP have background = NULL */
    const Background* background;
} Monitor;

struct _GreeterBackground
{
	GObject parent_instance;
	struct _GreeterBackgroundPrivate* priv;
};

struct _GreeterBackgroundClass
{
	GObjectClass parent_class;
};

typedef struct _GreeterBackgroundPrivate GreeterBackgroundPrivate;

struct _GreeterBackgroundPrivate
{
    GdkScreen* screen;
    gulong screen_monitors_changed_handler_id;
    /* one-window-gtk3-only
    GtkWindow* greeter_widget;
    */
    GSList* greeter_windows;

    /* Mapping monitor name <gchar*> to its config <MonitorConfig*> */
    GHashTable* configs;
    /* Default config for unlisted monitors */
    MonitorConfig* default_config;

	/* Array of configured monitors for current screen */
    Monitor* monitors;
    gsize monitors_size;
	/* Name => <Monitor*>, "Number" => <Monitor*> */
    GHashTable* monitors_map;

    GList* active_monitors_config;
    const Monitor* active_monitor;

    /* List of monitors <Monitor*> with user-background=true*/
    GSList* customized_monitors;

    /* List of monitors <Monitor*> with laptop=true */
    GSList* laptop_monitors;
    /* DBus proxy to catch lid state changing */
    GDBusProxy* laptop_upower_proxy;
    /* Cached lid state */
    gboolean laptop_lid_closed;

    /* Use cursor position to determinate current active monitor (dynamic) */
    gboolean follow_cursor;
    /* Use cursor position to determinate initial active monitor */
    gboolean follow_cursor_to_init;
};

enum
{
    BACKGROUND_SIGNAL_ACTIVE_MONITOR_CHANGED,
    BACKGROUND_SIGNAL_LAST
};

static guint background_signals[BACKGROUND_SIGNAL_LAST] = {0};

static const MonitorConfig DEFAULT_MONITOR_CONFIG =
{
    .bg =
    {
        .type = BACKGROUND_TYPE_COLOR,
        .options =
        {
            .color = {.red = 0.0, .green = 0.0, .blue = 0.0, .alpha = 1.0}
        }
    },
    .user_bg = TRUE,
    .laptop = FALSE
};

static const gchar* DBUS_UPOWER_NAME                = "org.freedesktop.UPower";
static const gchar* DBUS_UPOWER_PATH                = "/org/freedesktop/UPower";
static const gchar* DBUS_UPOWER_INTERFACE           = "org.freedesktop.UPower";
static const gchar* DBUS_UPOWER_PROP_LID_IS_PRESENT = "LidIsPresent";
static const gchar* DBUS_UPOWER_PROP_LID_IS_CLOSED  = "LidIsClosed";

static const gchar* ACTIVE_MONITOR_CURSOR_TAG       = "#cursor";

G_DEFINE_TYPE_WITH_PRIVATE(GreeterBackground, greeter_background, G_TYPE_OBJECT);

void greeter_background_set_active_monitor_config   (GreeterBackground* background,
                                                     const gchar* value);
void greeter_background_set_default_config          (GreeterBackground* background,
                                                     const gchar* bg,
                                                     gboolean user_bg,
                                                     gboolean laptop);
void greeter_background_set_monitor_config          (GreeterBackground* background,
                                                     const gchar* name,
                                                     const gchar* bg,
                                                     gboolean user_bg, gboolean user_bg_used,
                                                     gboolean laptop, gboolean laptop_used);
void greeter_background_remove_monitor_config       (GreeterBackground* background,
                                                     const gchar* name);
gchar** greeter_background_get_configured_monitors  (GreeterBackground* background);
void greeter_background_connect                     (GreeterBackground* background,
                                                     GdkScreen* screen);
void greeter_background_disconnect                  (GreeterBackground* background);
static gboolean greeter_background_find_monitor_data(GreeterBackground* background,
                                                     GHashTable* table,
                                                     const Monitor* monitor,
                                                     gpointer* data);
static void greeter_background_set_active_monitor   (GreeterBackground* background,
                                                     const Monitor* active);
static void greeter_background_get_cursor_position  (GreeterBackground* background,
                                                     gint* x, gint* y);
static void greeter_background_set_cursor_position  (GreeterBackground* background,
                                                     gint x, gint y);
static void greeter_background_try_init_dbus        (GreeterBackground* background);
static void greeter_background_stop_dbus            (GreeterBackground* background);
static gboolean greeter_background_monitor_enabled  (GreeterBackground* background,
                                                     const Monitor* monitor);
static void greeter_background_dbus_changed_cb      (GDBusProxy* proxy,
                                                     GVariant* changed_properties,
                                                     const gchar* const* invalidated_properties,
                                                     GreeterBackground* background);
static void greeter_background_monitors_changed_cb  (GdkScreen* screen,
                                                     GreeterBackground* background);

/* struct BackgroundConfig */
static gboolean background_config_initialize        (BackgroundConfig* config,
                                                     const gchar* value);
static void background_config_finalize              (BackgroundConfig* config);
static void background_config_copy                  (const BackgroundConfig* source,
                                                     BackgroundConfig* dest);

/* struct MonitorConfig */
static void monitor_config_free                     (MonitorConfig* config);
/* Copy source config to dest, return dest. Allocate memory if dest == NULL. */
static MonitorConfig* monitor_config_copy           (const MonitorConfig* source,
                                                     MonitorConfig* dest);

/* struct Background */
static gboolean background_initialize               (Background* bg,
                                                     const BackgroundConfig* config,
                                                     const Monitor* monitor,
                                                     GHashTable* images_cache);
static void background_finalize                     (Background* bg);

/* struct Monitor */
static void monitor_finalize                        (Monitor* info);
static void monitor_set_background                  (Monitor* monitor,
                                                     const Background* background);
static void monitor_draw_background                 (const Monitor* monitor,
                                                     cairo_t* cr);
static gboolean monitor_window_draw_cb              (GtkWidget* widget,
                                                     cairo_t* cr,
                                                     const Monitor* monitor);
static gboolean monitor_subwindow_draw_cb           (GtkWidget* widget,
                                                     cairo_t* cr,
                                                     GreeterBackground* background);
static gboolean monitor_window_enter_notify_cb      (GtkWidget* widget,
                                                     GdkEventCrossing* event,
                                                     const Monitor* monitor);

static GdkPixbuf* scale_image_file                  (const gchar* path,
                                                     ScalingMode mode,
                                                     gint width, gint height,
                                                     GHashTable* cache);
static GdkPixbuf* scale_image                       (GdkPixbuf* source,
                                                     ScalingMode mode,
                                                     gint width, gint height);
static cairo_surface_t* create_root_surface         (GdkScreen* screen);
static void set_root_pixmap_id                      (GdkScreen* screen,
                                                     Display* display,
                                                     Pixmap xpixmap);
static void set_surface_as_root                     (GdkScreen* screen,
                                                     cairo_surface_t* surface);


/* Implementation */

static void
greeter_background_class_init(GreeterBackgroundClass* klass)
{
	GObjectClass* gobject_class = G_OBJECT_CLASS(klass);

    background_signals[BACKGROUND_SIGNAL_ACTIVE_MONITOR_CHANGED] =
                            g_signal_new("active-monitor-changed",
                                         G_TYPE_FROM_CLASS(gobject_class),
                                         G_SIGNAL_RUN_FIRST,
                                         0, /* class_offset */
                                         NULL /* accumulator */, NULL /* accu_data */,
                                         g_cclosure_marshal_VOID__VOID,
                                         G_TYPE_NONE, 0);
}

static void
greeter_background_init(GreeterBackground* self)
{
    self->priv = G_TYPE_INSTANCE_GET_PRIVATE(self, GREETER_BACKGROUND_TYPE, GreeterBackgroundPrivate);
    self->priv->screen = NULL;
    self->priv->screen_monitors_changed_handler_id = 0;
    self->priv->greeter_windows = NULL;

    self->priv->configs = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, (GDestroyNotify)monitor_config_free);
    self->priv->default_config = monitor_config_copy(&DEFAULT_MONITOR_CONFIG, NULL);

    self->priv->monitors = NULL;
    self->priv->monitors_size = 0;
    self->priv->monitors_map = NULL;

    self->priv->customized_monitors = NULL;
    self->priv->active_monitors_config = NULL;
    self->priv->active_monitor = NULL;

    self->priv->laptop_monitors = NULL;
    self->priv->laptop_upower_proxy = NULL;
    self->priv->laptop_lid_closed = FALSE;
}

GreeterBackground* 
greeter_background_new(void)
{
	return GREETER_BACKGROUND(g_object_new(greeter_background_get_type(), NULL));
}

void
greeter_background_set_active_monitor_config(GreeterBackground* background,
                                             const gchar* value)
{
    g_return_if_fail(GREETER_IS_BACKGROUND(background));
    GreeterBackgroundPrivate* priv = background->priv;

    g_list_free_full(priv->active_monitors_config, g_free);
    priv->active_monitors_config = NULL;

    priv->follow_cursor = FALSE;
    priv->follow_cursor_to_init = FALSE;

    if(!value || !*value)
        return;

    gchar** iter;
    gchar** values = g_strsplit(value, ";", -1);

    for(iter = values; *iter; ++iter)
    {
        const gchar* value = *iter;
        if(g_strcmp0(value, ACTIVE_MONITOR_CURSOR_TAG) == 0)
        {
            priv->follow_cursor = TRUE;
            priv->follow_cursor_to_init = (priv->active_monitors_config == NULL);
        }
        else
            priv->active_monitors_config = g_list_prepend(priv->active_monitors_config, g_strdup(value));
    }
    g_strfreev(values);

    priv->active_monitors_config = g_list_reverse(priv->active_monitors_config);
}

void
greeter_background_set_default_config(GreeterBackground* background,
                                      const gchar* bg,
                                      gboolean user_bg,
                                      gboolean laptop)
{
    g_return_if_fail(GREETER_IS_BACKGROUND(background));
    GreeterBackgroundPrivate* priv = background->priv;

    if(priv->default_config)
        monitor_config_free(priv->default_config);

    priv->default_config = g_new0(MonitorConfig, 1);
    if(!background_config_initialize(&priv->default_config->bg, bg))
        background_config_copy(&DEFAULT_MONITOR_CONFIG.bg, &priv->default_config->bg);
    priv->default_config->user_bg = user_bg;
    priv->default_config->laptop = laptop;
}

void
greeter_background_set_monitor_config(GreeterBackground* background,
                                      const gchar* name,
                                      const gchar* bg,
                                      gboolean user_bg, gboolean user_bg_used,
                                      gboolean laptop, gboolean laptop_used)
{
    g_return_if_fail(GREETER_IS_BACKGROUND(background));
    GreeterBackgroundPrivate* priv = background->priv;

    MonitorConfig* config = g_new0(MonitorConfig, 1);

    if(!background_config_initialize(&config->bg, bg))
        background_config_copy(&priv->default_config->bg, &config->bg);
    config->user_bg = user_bg_used ? user_bg : priv->default_config->user_bg;
    config->laptop = laptop_used ? laptop : priv->default_config->laptop;

    g_hash_table_insert(priv->configs, g_strdup(name), config);
}

void
greeter_background_remove_monitor_config(GreeterBackground* background,
                                         const gchar* name)
{
    g_return_if_fail(GREETER_IS_BACKGROUND(background));
    g_hash_table_remove(background->priv->configs, name);
}

gchar**
greeter_background_get_configured_monitors(GreeterBackground* background)
{
    g_return_if_fail(GREETER_IS_BACKGROUND(background));
    GreeterBackgroundPrivate* priv = background->priv;

    gint n = g_hash_table_size(priv->configs);
    gchar** names = g_new(gchar*, n + 1);
    names[n--] = NULL;

    gpointer key;
    GHashTableIter iter;
    g_hash_table_iter_init(&iter, priv->configs);
    while(g_hash_table_iter_next(&iter, &key, NULL))
        names[n--] = g_strdup(key);

    return names;
}

void
greeter_background_connect(GreeterBackground* background,
                           GdkScreen* screen)
{
    g_return_if_fail(GREETER_IS_BACKGROUND(background));
    g_return_if_fail(GDK_IS_SCREEN(screen));

    g_debug("Connecting to screen");

    GreeterBackgroundPrivate* priv = background->priv;
    gulong screen_monitors_changed_handler_id = (priv->screen == screen) ? priv->screen_monitors_changed_handler_id : 0;
    if(screen_monitors_changed_handler_id)
        priv->screen_monitors_changed_handler_id = 0;

    if(priv->screen)
        greeter_background_disconnect(background);

    priv->screen = screen;
    priv->monitors_size = gdk_screen_get_n_monitors(screen);
    priv->monitors = g_new0(Monitor, priv->monitors_size);
    priv->monitors_map = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

    /* Used to track situation when all monitors marked as "#skip" */
    Monitor* first_not_skipped_monitor = NULL;

    GHashTable* images_cache = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_object_unref);
    gint i;
    for(i = 0; i < priv->monitors_size; ++i)
    {
        const MonitorConfig* config;
        Monitor* monitor = &priv->monitors[i];

        monitor->object = background;
        monitor->name = g_strdup(gdk_screen_get_monitor_plug_name(screen, i));
        monitor->number = i;

        const gchar* printable_name = monitor->name ? monitor->name : "<unknown>";

        if(!greeter_background_find_monitor_data(background, priv->configs, monitor, (gpointer*)&config))
        {
            g_debug("No configuration options for monitor %s #%d, using default", printable_name, i);
            config = priv->default_config;
        }

        gdk_screen_get_monitor_geometry(screen, i, &monitor->geometry);

        g_debug("Monitor: %s #%d (%dx%d at %dx%d)%s", printable_name, i,
                monitor->geometry.width, monitor->geometry.height,
                monitor->geometry.x, monitor->geometry.y,
                (i == gdk_screen_get_primary_monitor(screen)) ? " primary" : "");

        /* Force last skipped monitor to be active monitor, if there is no other choice */
        if(config->bg.type == BACKGROUND_TYPE_SKIP)
        {
            if(i < priv->monitors_size - 1 || first_not_skipped_monitor)
                continue;
            g_debug("Monitor %s #%d can not be skipped, using default configuration for it", printable_name, i);
            if(priv->default_config->bg.type != BACKGROUND_TYPE_SKIP)
                config = priv->default_config;
            else
                config = &DEFAULT_MONITOR_CONFIG;
        }

        if(!first_not_skipped_monitor)
            first_not_skipped_monitor = monitor;

        monitor->window = GTK_WINDOW(gtk_window_new(GTK_WINDOW_TOPLEVEL));
        gtk_window_set_type_hint(monitor->window, GDK_WINDOW_TYPE_HINT_DESKTOP);
        gtk_window_set_keep_below(monitor->window, TRUE);
        gtk_window_set_resizable(monitor->window, FALSE);
        gtk_widget_set_app_paintable(GTK_WIDGET(monitor->window), TRUE);
        gtk_window_set_screen(monitor->window, screen);
        gtk_widget_set_size_request(GTK_WIDGET(monitor->window), monitor->geometry.width, monitor->geometry.height);
        gtk_window_move(monitor->window, monitor->geometry.x, monitor->geometry.y);
        gtk_widget_show(GTK_WIDGET(monitor->window));
        monitor->window_draw_handler_id = g_signal_connect(G_OBJECT(monitor->window), "draw",
                                                           G_CALLBACK(monitor_window_draw_cb),
                                                           monitor);
        if(priv->follow_cursor)
            g_signal_connect(G_OBJECT(monitor->window), "enter-notify-event",
                             G_CALLBACK(monitor_window_enter_notify_cb), monitor);

        if(config->user_bg)
            priv->customized_monitors = g_slist_prepend(priv->customized_monitors, monitor);

        if(config->laptop)
            priv->laptop_monitors = g_slist_prepend(priv->laptop_monitors, monitor);

        if(!background_initialize(&monitor->background_configured, &config->bg, monitor, images_cache))
            background_initialize(&monitor->background_configured, &DEFAULT_MONITOR_CONFIG.bg, monitor, images_cache);
        monitor_set_background(monitor, &monitor->background_configured);

        if(monitor->name)
            g_hash_table_insert(priv->monitors_map, g_strdup(monitor->name), monitor);
        g_hash_table_insert(priv->monitors_map, g_strdup_printf("%d", i), monitor);
    }
    g_hash_table_unref(images_cache);

    if(priv->laptop_monitors && !priv->laptop_upower_proxy)
        greeter_background_try_init_dbus(background);
    else if(!priv->laptop_monitors)
        greeter_background_stop_dbus(background);

    if(priv->follow_cursor_to_init)
    {
        gint x, y;
        greeter_background_get_cursor_position(background, &x, &y);
        for(i = 0; i < priv->monitors_size && !priv->active_monitor; ++i)
        {
            const Monitor* monitor = &priv->monitors[i];
            if(greeter_background_monitor_enabled(background, monitor) &&
               x >= monitor->geometry.x && x < monitor->geometry.x + monitor->geometry.width &&
               y >= monitor->geometry.y && y < monitor->geometry.y + monitor->geometry.height)
                greeter_background_set_active_monitor(background, monitor);
        }
    }
    if(!priv->active_monitor)
        greeter_background_set_active_monitor(background, NULL);

    if(screen_monitors_changed_handler_id)
        priv->screen_monitors_changed_handler_id = screen_monitors_changed_handler_id;
    else
        priv->screen_monitors_changed_handler_id = g_signal_connect(G_OBJECT(screen), "monitors-changed",
                                                                    G_CALLBACK(greeter_background_monitors_changed_cb),
                                                                    background);
}

void
greeter_background_disconnect(GreeterBackground* background)
{
    g_return_if_fail(GREETER_IS_BACKGROUND(background));
    GreeterBackgroundPrivate* priv = background->priv;

    priv->screen = NULL;
    priv->active_monitor = NULL;

    if(priv->screen_monitors_changed_handler_id)
        g_signal_handler_disconnect(priv->screen, priv->screen_monitors_changed_handler_id);
    priv->screen_monitors_changed_handler_id = 0;

    gint i;
    for(i = 0; i < priv->monitors_size; ++i)
        monitor_finalize(&priv->monitors[i]);
    g_free(priv->monitors);
    priv->monitors = NULL;
    priv->monitors_size = 0;

    g_hash_table_unref(priv->monitors_map);
    priv->monitors_map = NULL;
    g_slist_free(priv->customized_monitors);
    priv->customized_monitors = NULL;
    g_slist_free(priv->laptop_monitors);
    priv->laptop_monitors = NULL;
}

/* Moved to separate function to simplify needless and unnecessary syntax expansion in future (regex) */
static gboolean
greeter_background_find_monitor_data(GreeterBackground* background,
                                     GHashTable* table,
                                     const Monitor* monitor,
                                     gpointer* data)
{
    if(!monitor->name || !g_hash_table_lookup_extended(table, monitor->name, NULL, data))
    {
        gchar* num_str = g_strdup_printf("%d", monitor->number);
        gboolean result = g_hash_table_lookup_extended(table, num_str, NULL, data);
        g_free(num_str);
        return result;
    }
    return TRUE;
}

static void
greeter_background_set_active_monitor(GreeterBackground* background,
                                      const Monitor* active)
{
    GreeterBackgroundPrivate* priv = background->priv;

    if(active && !active->background)
    {
        if(priv->active_monitor)
            return;
        active = NULL;
    }

    /* Auto */
    if(!active)
    {
        /* Normal way: at least one configured active monitor is not disabled */
        GList* iter;
        for(iter = priv->active_monitors_config; iter && !active; iter = g_list_next(iter))
        {
            const Monitor* monitor = g_hash_table_lookup(priv->monitors_map, iter->data);
            if(monitor && monitor->background && greeter_background_monitor_enabled(background, monitor))
                active = monitor;
        }

        /* All monitors listed in active-monitor-config are disabled (or option is empty) */

        /* Using primary monitor */
        if(!active)
        {
            active = &priv->monitors[gdk_screen_get_primary_monitor(priv->screen)];
            if(!active->background || !greeter_background_monitor_enabled(background, active))
                active = NULL;
        }

        /* Fallback: first enabled and/or not skipped monitor (screen always have one) */
        if(!active)
        {
            gint i;
            const Monitor* first_not_skipped = NULL;
            for(i = 0; i < priv->monitors_size && !active; ++i)
            {
                const Monitor* monitor = &priv->monitors[i];
                if(!monitor->background)
                    continue;
                if(greeter_background_monitor_enabled(background, monitor))
                    active = monitor;
                if(!first_not_skipped)
                    first_not_skipped = active;
            }
            if(!active)
                active = first_not_skipped;
        }
    }

    if(active == priv->active_monitor)
        return;

    priv->active_monitor = active;
    g_debug("Active monitor changed to: %s #%d", active->name, active->number);
    g_signal_emit(background, background_signals[BACKGROUND_SIGNAL_ACTIVE_MONITOR_CHANGED], 0);

    gint x, y;
    greeter_background_get_cursor_position(background, &x, &y);
    /* Do not center cursor if it is already inside active monitor */
    if(x < active->geometry.x || x >= active->geometry.x + active->geometry.width ||
       y < active->geometry.y || y >= active->geometry.y + active->geometry.height)
        greeter_background_set_cursor_position(background,
                                               active->geometry.x + active->geometry.width/2,
                                               active->geometry.y + active->geometry.height/2);

    /* Update greeter windows */
    GSList* iter;
    for(iter = priv->greeter_windows; iter; iter = g_slist_next(iter))
    {
        gtk_window_set_screen(GTK_WINDOW(iter->data), priv->screen);
        if(gtk_widget_get_visible(GTK_WIDGET(iter->data)))
        {   /* Toggle window visibility to place window above of any 'background' windows */
            gtk_widget_hide(GTK_WIDGET(iter->data));
            gtk_widget_show(GTK_WIDGET(iter->data));
            gtk_widget_queue_resize(GTK_WIDGET(iter->data));
        }
    }
}

static void
greeter_background_get_cursor_position(GreeterBackground* background,
                                       gint* x, gint* y)
{
    GreeterBackgroundPrivate* priv = background->priv;

    GdkDisplay* display = gdk_screen_get_display(priv->screen);
    GdkDeviceManager* device_manager = gdk_display_get_device_manager(display);
    GdkDevice* device = gdk_device_manager_get_client_pointer(device_manager);
    gdk_device_get_position(device, NULL, x, y);
}

static void
greeter_background_set_cursor_position(GreeterBackground* background,
                                       gint x, gint y)
{
    GreeterBackgroundPrivate* priv = background->priv;

    GdkDisplay* display = gdk_screen_get_display(priv->screen);
    GdkDeviceManager* device_manager = gdk_display_get_device_manager(display);
    gdk_device_warp(gdk_device_manager_get_client_pointer(device_manager), priv->screen, x, y);
}

static void
greeter_background_try_init_dbus(GreeterBackground* background)
{
    g_debug("Creating DBus proxy");
    GError* error = NULL;
    GreeterBackgroundPrivate* priv = background->priv;

    if(priv->laptop_upower_proxy)
        greeter_background_stop_dbus(background);

    priv->laptop_upower_proxy = g_dbus_proxy_new_for_bus_sync(
                                            G_BUS_TYPE_SYSTEM,
                                            G_DBUS_PROXY_FLAGS_NONE,
                                            NULL,   /* interface info */
                                            DBUS_UPOWER_NAME,
                                            DBUS_UPOWER_PATH,
                                            DBUS_UPOWER_INTERFACE,
                                            NULL,   /* cancellable */
                                            &error);
    if(!priv->laptop_upower_proxy)
    {
        if(error)
            g_warning("Failed to create dbus proxy: %s", error->message);
        g_clear_error(&error);
        return;
    }

    GVariant* variant = g_dbus_proxy_get_cached_property(priv->laptop_upower_proxy, DBUS_UPOWER_PROP_LID_IS_PRESENT);
    gboolean lid_present = g_variant_get_boolean(variant);
    g_variant_unref(variant);

    g_debug("UPower.%s property value: %d", DBUS_UPOWER_PROP_LID_IS_PRESENT, lid_present);

    if(!lid_present)
        greeter_background_stop_dbus(background);
    else
    {
        variant = g_dbus_proxy_get_cached_property(priv->laptop_upower_proxy, DBUS_UPOWER_PROP_LID_IS_CLOSED);
        priv->laptop_lid_closed = g_variant_get_boolean(variant);
        g_variant_unref(variant);

        g_signal_connect(priv->laptop_upower_proxy, "g-properties-changed",
                         G_CALLBACK(greeter_background_dbus_changed_cb), background);
    }
}

static void
greeter_background_stop_dbus(GreeterBackground* background)
{
    if(!background->priv->laptop_upower_proxy)
        return;
    g_clear_object(&background->priv->laptop_upower_proxy);
}

static gboolean
greeter_background_monitor_enabled(GreeterBackground* background,
                                   const Monitor* monitor)
{
    GreeterBackgroundPrivate* priv = background->priv;

    if(priv->laptop_upower_proxy && g_slist_find(priv->laptop_monitors, monitor))
        return !priv->laptop_lid_closed;
    return TRUE;
}

static void
greeter_background_dbus_changed_cb(GDBusProxy* proxy,
                                   GVariant* changed_properties,
                                   const gchar* const* invalidated_properties,
                                   GreeterBackground* background)
{
    g_return_if_fail(GREETER_IS_BACKGROUND(background));
    GreeterBackgroundPrivate* priv = background->priv;

    GVariant* variant = g_dbus_proxy_get_cached_property(priv->laptop_upower_proxy, DBUS_UPOWER_PROP_LID_IS_CLOSED);
    gboolean new_state = g_variant_get_boolean(variant);
    g_variant_unref(variant);

    if(new_state == priv->laptop_lid_closed)
        return;

    g_debug("UPower: lid state changed to '%s'", priv->laptop_lid_closed ? "closed" : "opened");

    priv->laptop_lid_closed = new_state;
    if(priv->laptop_monitors)
    {
        if(!priv->follow_cursor || (new_state && priv->laptop_monitors->data == priv->active_monitor))
            greeter_background_set_active_monitor(background, NULL);
    }
}

static void
greeter_background_monitors_changed_cb(GdkScreen* screen,
                                       GreeterBackground* background)
{
    g_return_if_fail(GREETER_IS_BACKGROUND(background));
    greeter_background_connect(background, screen);
}

void greeter_background_add_subwindow(GreeterBackground* background,
                                      GtkWindow* window)
{
    g_return_if_fail(GREETER_IS_BACKGROUND(background));
    g_return_if_fail(GTK_IS_WINDOW(window));

    GreeterBackgroundPrivate* priv = background->priv;

    if(!g_slist_find(priv->greeter_windows, window))
    {
        priv->greeter_windows = g_slist_prepend(priv->greeter_windows, window);
        g_signal_connect(G_OBJECT(window), "draw", G_CALLBACK(monitor_subwindow_draw_cb), background);
    }

    if(priv->screen)
        gtk_window_set_screen(window, priv->screen);
}

void greeter_background_remove_subwindow(GreeterBackground* background,
                                         GtkWindow* window)
{
    g_return_if_fail(GREETER_IS_BACKGROUND(background));
    g_return_if_fail(GTK_IS_WINDOW(window));

    GreeterBackgroundPrivate* priv = background->priv;

    GSList* item = g_slist_find(priv->greeter_windows, window);
    if(item)
    {
        g_object_disconnect(G_OBJECT(window),
                            "any-signal", G_CALLBACK(monitor_subwindow_draw_cb), background,
                            NULL);
        priv->greeter_windows = g_slist_delete_link(priv->greeter_windows, item);
    }
}

void
greeter_background_set_custom_background(GreeterBackground* background,
                                         const gchar* value)
{
    g_return_if_fail(GREETER_IS_BACKGROUND(background));

    GreeterBackgroundPrivate* priv = background->priv;
    if(!priv->customized_monitors)
        return;

    BackgroundConfig config;
    background_config_initialize(&config, value);

    GHashTable *images_cache = NULL;
    if(config.type == BACKGROUND_TYPE_IMAGE)
        images_cache = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_object_unref);

    GSList* iter;
    for(iter = priv->customized_monitors; iter; iter = g_slist_next(iter))
    {
        Monitor *monitor = iter->data;

        background_finalize(&monitor->background_custom);
        if(config.type != BACKGROUND_TYPE_INVALID &&
           background_initialize(&monitor->background_custom, &config, monitor, images_cache))
            monitor_set_background(monitor, &monitor->background_custom);
        else
            monitor_set_background(monitor, &monitor->background_configured);
    }
    if(images_cache)
        g_hash_table_unref(images_cache);
    if(config.type != BACKGROUND_TYPE_INVALID)
        background_config_finalize(&config);

    for(iter = priv->greeter_windows; iter; iter = g_slist_next(iter))
        gtk_widget_queue_draw(GTK_WIDGET(iter->data));
}

void
greeter_background_save_xroot(GreeterBackground* background)
{
    g_return_if_fail(GREETER_IS_BACKGROUND(background));

    GreeterBackgroundPrivate* priv = background->priv;
    cairo_surface_t* surface = create_root_surface(priv->screen);
    cairo_t* cr = cairo_create(surface);
    gsize i;

    for(i = 0; i <= priv->monitors_size; ++i)
    {
        const Monitor* monitor = &priv->monitors[i];
        if(monitor == priv->active_monitor || !monitor->background)
            continue;
        if(i == priv->monitors_size)
            monitor = priv->active_monitor;
        cairo_save(cr);
        cairo_translate(cr, monitor->geometry.x, monitor->geometry.y);
        monitor_draw_background(monitor, cr);
        cairo_restore(cr);
    }
    set_surface_as_root(priv->screen, surface);

    cairo_destroy(cr);
    cairo_surface_destroy(surface);
}

const GdkRectangle*
greeter_background_get_active_monitor_geometry(GreeterBackground* background)
{
    g_return_if_fail(GREETER_IS_BACKGROUND(background));
    GreeterBackgroundPrivate* priv = background->priv;

    return priv->active_monitor ? &priv->active_monitor->geometry : NULL;
}

static gboolean
background_config_initialize(BackgroundConfig* config,
                             const gchar* value)
{
    config->type = BACKGROUND_TYPE_INVALID;
    if(!value || strlen(value) == 0)
        return FALSE;
    if(g_strcmp0(value, BACKGROUND_TYPE_SKIP_VALUE) == 0)
        config->type = BACKGROUND_TYPE_SKIP;
    else if(gdk_rgba_parse(&config->options.color, value))
        config->type = BACKGROUND_TYPE_COLOR;
    else
    {
        const gchar** prefix = SCALING_MODE_PREFIXES;
        while(*prefix && !g_str_has_prefix(value, *prefix))
            ++prefix;

        if(*prefix)
        {
            config->options.image.mode = (ScalingMode)(prefix - SCALING_MODE_PREFIXES);
            value += strlen(*prefix);
        }
        else
            config->options.image.mode = SCALING_MODE_ZOOMED;

        config->options.image.path = g_strdup(value);
        config->type = BACKGROUND_TYPE_IMAGE;        
    }
    return TRUE;
}

static void
background_config_finalize(BackgroundConfig* config)
{
    if(config->type == BACKGROUND_TYPE_IMAGE)
        g_free(config->options.image.path);
    config->type = BACKGROUND_TYPE_INVALID;
}

static void
background_config_copy(const BackgroundConfig* source,
                       BackgroundConfig* dest)
{
    *dest = *source;
    if(source->type == BACKGROUND_TYPE_IMAGE)
        dest->options.image.path = g_strdup(source->options.image.path);
}

static void
monitor_config_free(MonitorConfig* config)
{
    background_config_finalize(&config->bg);
    g_free(config);
}

static MonitorConfig* monitor_config_copy(const MonitorConfig* source,
                                          MonitorConfig* dest)
{
    if(!dest)
        dest = g_new0(MonitorConfig, 1);
    background_config_copy(&source->bg, &dest->bg);
    dest->user_bg = source->user_bg;
    dest->laptop = source->laptop;
    return dest;
}

static gboolean
background_initialize(Background* bg,
                      const BackgroundConfig* config,
                      const Monitor* monitor,
                      GHashTable* images_cache)
{
    if(config->type == BACKGROUND_TYPE_IMAGE)
    {
        GdkPixbuf* pixbuf = scale_image_file(config->options.image.path,
                                             config->options.image.mode,
                                             monitor->geometry.width, monitor->geometry.height,
                                             images_cache);
        if(!pixbuf)
        {
            g_warning("Failed to read wallpaper: %s", config->options.image.path);
            return FALSE;
        }
        bg->options.image = pixbuf;
    }
    else if(config->type == BACKGROUND_TYPE_COLOR)
        bg->options.color = config->options.color;
    else
        return FALSE;
    bg->type = config->type;
    return TRUE;
}

static void
background_finalize(Background* bg)
{
    if(bg->type == BACKGROUND_TYPE_IMAGE)
        g_clear_object(&bg->options.image);
    bg->type = BACKGROUND_TYPE_INVALID;
}

static void
monitor_set_background(Monitor* monitor,
                       const Background* background)
{
    monitor->background = background;
    gtk_widget_queue_draw(GTK_WIDGET(monitor->window));
}

static void
monitor_finalize(Monitor* monitor)
{
    background_finalize(&monitor->background_configured);
    background_finalize(&monitor->background_custom);
    g_free(monitor->name);
    if(monitor->window_draw_handler_id)
        g_signal_handler_disconnect(monitor->window, monitor->window_draw_handler_id);
    if(monitor->window)
        gtk_widget_destroy(GTK_WIDGET(monitor->window));
    monitor->name = NULL;
    monitor->window = NULL;
    monitor->window_draw_handler_id = 0;
}

static void
monitor_draw_background(const Monitor* monitor,
                        cairo_t* cr)
{
    g_return_if_fail(monitor != NULL);
    g_return_if_fail(monitor->background != NULL);

    if(monitor->background->type == BACKGROUND_TYPE_IMAGE && monitor->background->options.image)
    {
        gdk_cairo_set_source_pixbuf(cr, monitor->background->options.image, 0, 0);
        cairo_paint(cr);
    }
    else if(monitor->background->type == BACKGROUND_TYPE_COLOR)
    {
        cairo_rectangle(cr, 0, 0, monitor->geometry.width, monitor->geometry.height);
        gdk_cairo_set_source_rgba(cr, &monitor->background->options.color);
        cairo_fill(cr);
    }
}

static gboolean
monitor_window_draw_cb(GtkWidget* widget,
                       cairo_t* cr,
                       const Monitor* monitor)
{
    if(monitor->background)
        monitor_draw_background(monitor, cr);
    return FALSE;
}

static gboolean
monitor_subwindow_draw_cb(GtkWidget* widget,
                          cairo_t* cr,
                          GreeterBackground* background)
{
    g_return_val_if_fail(GREETER_IS_BACKGROUND(background), FALSE);
    if(background->priv->active_monitor)
    {
        const GdkRectangle* geometry = &background->priv->active_monitor->geometry;
        gint x = 0, y = 0;
        gtk_window_get_position(GTK_WINDOW(widget), &x, &y);

        cairo_save(cr);
        cairo_translate(cr, geometry->x - x, geometry->y - y);
        monitor_draw_background(background->priv->active_monitor, cr);
        cairo_restore(cr);
    }
    return FALSE;
}

static gboolean
monitor_window_enter_notify_cb(GtkWidget* widget,
                               GdkEventCrossing* event,
                               const Monitor* monitor)
{
    if(monitor->object->priv->active_monitor != monitor &&
       greeter_background_monitor_enabled(monitor->object, monitor))
        greeter_background_set_active_monitor(monitor->object, monitor);
    return FALSE;
}

static GdkPixbuf*
scale_image_file(const gchar* path,
                 ScalingMode mode,
                 gint width, gint height,
                 GHashTable* cache)
{
    gchar* key = NULL;
    GdkPixbuf* pixbuf = NULL;
    if(cache)
    {
        key = g_strdup_printf("%s\n%d %dx%d", path, mode, width, height);
        if (g_hash_table_lookup_extended(cache, key, NULL, (gpointer*)&pixbuf))
            return GDK_PIXBUF(g_object_ref(pixbuf));
    }

    if (!cache || !g_hash_table_lookup_extended(cache, path, NULL, (gpointer*)&pixbuf))
    {
        GError *error = NULL;
        pixbuf = gdk_pixbuf_new_from_file(path, &error);
        if(error)
        {
            g_warning("Failed to load background: %s", error->message);
            g_clear_error(&error);
        }
        else if(cache)
            g_hash_table_insert(cache, g_strdup(path), g_object_ref (pixbuf));
    }

    if(pixbuf)
    {
        GdkPixbuf* scaled = scale_image(pixbuf, mode, width, height);
        if (cache)
            g_hash_table_insert(cache, g_strdup(key), g_object_ref(scaled));
        g_object_unref(pixbuf);
        pixbuf = scaled;
    }

    return pixbuf;
}

static GdkPixbuf*
scale_image(GdkPixbuf* source,
            ScalingMode mode,
            gint width, gint height)
{
    if(mode == SCALING_MODE_ZOOMED)
    {
        gint offset_x = 0;
        gint offset_y = 0;
        gint p_width = gdk_pixbuf_get_width(source);
        gint p_height = gdk_pixbuf_get_height(source);
        gdouble scale_x = (gdouble)width / p_width;
        gdouble scale_y = (gdouble)height / p_height;

        if(scale_x < scale_y)
        {
            scale_x = scale_y;
            offset_x = (width - (p_width * scale_x)) / 2;
        }
        else
        {
            scale_y = scale_x;
            offset_y = (height - (p_height * scale_y)) / 2;
        }

        GdkPixbuf *pixbuf = gdk_pixbuf_new(GDK_COLORSPACE_RGB, TRUE,
                                           gdk_pixbuf_get_bits_per_sample (source),
                                           width, height);
        gdk_pixbuf_composite(source, pixbuf, 0, 0, width, height,
                             offset_x, offset_y, scale_x, scale_y, GDK_INTERP_BILINEAR, 0xFF);
        return pixbuf;
    }
    else if(mode == SCALING_MODE_STRETCHED)
    {
        return gdk_pixbuf_scale_simple(source, width, height, GDK_INTERP_BILINEAR);
    }
    return GDK_PIXBUF(g_object_ref(source));
}

/* The following code for setting a RetainPermanent background pixmap was taken
   originally from Gnome, with some fixes from MATE. see:
   https://github.com/mate-desktop/mate-desktop/blob/master/libmate-desktop/mate-bg.c */
static cairo_surface_t*
create_root_surface(GdkScreen* screen)
{
    gint number, width, height;
    Display *display;
    Pixmap pixmap;
    cairo_surface_t *surface;

    number = gdk_screen_get_number (screen);
    width = gdk_screen_get_width (screen);
    height = gdk_screen_get_height (screen);

    /* Open a new connection so with Retain Permanent so the pixmap remains when the greeter quits */
    gdk_flush ();
    display = XOpenDisplay (gdk_display_get_name (gdk_screen_get_display (screen)));
    if (!display)
    {
        g_warning ("Failed to create root pixmap");
        return NULL;
    }

    XSetCloseDownMode (display, RetainPermanent);
    pixmap = XCreatePixmap (display, RootWindow (display, number), width, height, DefaultDepth (display, number));
    XCloseDisplay (display);

    /* Convert into a Cairo surface */
    surface = cairo_xlib_surface_create (GDK_SCREEN_XDISPLAY (screen),
                                         pixmap,
                                         GDK_VISUAL_XVISUAL (gdk_screen_get_system_visual (screen)),
                                         width, height);

    return surface;
}

/* Sets the "ESETROOT_PMAP_ID" property to later be used to free the pixmap */
static void
set_root_pixmap_id(GdkScreen* screen,
                   Display* display,
                   Pixmap xpixmap)
{
    
    Window xroot = RootWindow (display, gdk_screen_get_number (screen));
    char *atom_names[] = {"_XROOTPMAP_ID", "ESETROOT_PMAP_ID"};
    Atom atoms[G_N_ELEMENTS(atom_names)] = {0};

    Atom type;
    int format;
    unsigned long nitems, after;
    unsigned char *data_root, *data_esetroot;

    /* Get atoms for both properties in an array, only if they exist.
     * This method is to avoid multiple round-trips to Xserver
     */
    if (XInternAtoms (display, atom_names, G_N_ELEMENTS(atom_names), True, atoms) &&
        atoms[0] != None && atoms[1] != None)
    {

        XGetWindowProperty (display, xroot, atoms[0], 0L, 1L, False, AnyPropertyType,
                            &type, &format, &nitems, &after, &data_root);
        if (data_root && type == XA_PIXMAP && format == 32 && nitems == 1)
        {
            XGetWindowProperty (display, xroot, atoms[1], 0L, 1L, False, AnyPropertyType,
                                &type, &format, &nitems, &after, &data_esetroot);
            if (data_esetroot && type == XA_PIXMAP && format == 32 && nitems == 1)
            {
                Pixmap xrootpmap = *((Pixmap *) data_root);
                Pixmap esetrootpmap = *((Pixmap *) data_esetroot);
                XFree (data_root);
                XFree (data_esetroot);

                gdk_error_trap_push ();
                if (xrootpmap && xrootpmap == esetrootpmap) {
                    XKillClient (display, xrootpmap);
                }
                if (esetrootpmap && esetrootpmap != xrootpmap) {
                    XKillClient (display, esetrootpmap);
                }

                XSync (display, False);
                gdk_error_trap_pop_ignored ();
            }
        }
    }

    /* Get atoms for both properties in an array, create them if needed.
     * This method is to avoid multiple round-trips to Xserver
     */
    if (!XInternAtoms (display, atom_names, G_N_ELEMENTS(atom_names), False, atoms) ||
        atoms[0] == None || atoms[1] == None) {
        g_warning("Could not create atoms needed to set root pixmap id/properties.\n");
        return;
    }

    /* Set new _XROOTMAP_ID and ESETROOT_PMAP_ID properties */
    XChangeProperty (display, xroot, atoms[0], XA_PIXMAP, 32,
                     PropModeReplace, (unsigned char *) &xpixmap, 1);

    XChangeProperty (display, xroot, atoms[1], XA_PIXMAP, 32,
                     PropModeReplace, (unsigned char *) &xpixmap, 1);
}

/**
* set_surface_as_root:
* @screen: the #GdkScreen to change root background on
* @surface: the #cairo_surface_t to set root background from.
* Must be an xlib surface backing a pixmap.
*
* Set the root pixmap, and properties pointing to it. We
* do this atomically with a server grab to make sure that
* we won't leak the pixmap if somebody else it setting
* it at the same time. (This assumes that they follow the
* same conventions we do). @surface should come from a call
* to create_root_surface().
**/
static void
set_surface_as_root(GdkScreen* screen,
                    cairo_surface_t* surface)
{
    g_return_if_fail(cairo_surface_get_type (surface) == CAIRO_SURFACE_TYPE_XLIB);

    /* Desktop background pixmap should be created from dummy X client since most
     * applications will try to kill it with XKillClient later when changing pixmap
     */
    Display *display = GDK_DISPLAY_XDISPLAY (gdk_screen_get_display (screen));
    Pixmap pixmap_id = cairo_xlib_surface_get_drawable (surface);
    Window xroot = RootWindow (display, gdk_screen_get_number(screen));

    XGrabServer (display);

    XSetWindowBackgroundPixmap (display, xroot, pixmap_id);
    set_root_pixmap_id (screen, display, pixmap_id);
    XClearWindow (display, xroot);

    XFlush (display);
    XUngrabServer (display);
}
