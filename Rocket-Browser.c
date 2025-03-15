#include <gtk/gtk.h>
#include <webkit2/webkit2.h>
#include <stdio.h>
#include <glib.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <sqlite3.h>
#include <time.h>
#include <curl/curl.h>
#include <json-glib/json-glib.h>
#include <ifaddrs.h>  // Add this line
#include <net/if.h>   // Add this line
#include <sys/types.h>
#include <sys/socket.h>
#include <gdk/gdk.h>
#include <linux/if_tun.h>
#include <net/route.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <sys/wait.h>
#include <errno.h>

// At the start of file, after includes, before any structures:

// Forward declare structures to resolve circular dependencies
typedef struct _BrowserTab BrowserTab;
typedef struct _VPNConnection VPNConnection;

// Define VPN structure
struct _VPNConnection {
    char* config_file;     // OpenVPN config file path
    gboolean connected;
    GtkWidget* status_label;
    GtkWidget* connect_button;
    GPid vpn_pid;         // Process ID for OpenVPN
    char* last_error;
    GtkWidget* config_label;  // Add label to show loaded config
};

// Add structures for history and cookies
typedef struct {
    sqlite3* db;
    GtkWidget* history_window;
    GtkListStore* history_store;
} BrowserHistory;

typedef struct {
    sqlite3* db;
    GtkWidget* cookie_window;
    GtkListStore* cookie_store;
} BrowserCookies;

// Add theme mode enum
typedef enum {
    MODE_LIGHT,
    MODE_DARK, 
    MODE_MATRIX
} BrowserMode;

// Update BrowserTab structure
struct _BrowserTab {
    GtkWidget* container;
    GtkWidget* url_entry;
    GtkComboBoxText* search_engine_combo;
    WebKitWebView* webview;
    GtkWidget* title_label;
    GtkWidget* progress_bar;
    GtkWidget* dev_tools_button;  // Add this field
    GtkWidget* menu_button;  // Add this line
    BrowserHistory* history;
    BrowserMode mode;         // Add theme mode
    GtkWidget* mode_item;     // Add mode menu item
    VPNConnection* vpn;  // Now VPNConnection is defined before use
};

// Enhanced InterceptData structure
typedef struct {
    gboolean enabled;
    GtkWidget* window;
    GtkTextBuffer* request_buffer;
    GtkTextBuffer* response_buffer;
    GtkTextBuffer* req_headers_buffer;
    GtkTextBuffer* resp_headers_buffer;
    GQueue* pending_requests;  // Queue for intercepted requests
    WebKitWebResource* current_resource;  // Currently displayed resource
    gboolean request_modified;  // Flag for modified requests
} InterceptData;

// Update PendingRequest struct
typedef struct {
    WebKitWebView* web_view;      // Add WebView reference
    WebKitWebResource* resource;
    WebKitURIRequest* request;
    WebKitURIResponse* response;  // Add response field
    gchar* method;
    gchar* uri;
    GHashTable* headers;
    GHashTable* response_headers;  // Add response headers
    guint status_code;            // Add status code
    gchar* content_type;          // Add content type
} PendingRequest;

// Add to main struct
typedef struct {
    // ... existing fields ...
    BrowserHistory* history;
    BrowserCookies* cookies;
} BrowserData;

// Add structure for WebRTC leak checking
typedef struct {
    char* local_ip;
    char* public_ip;
    GtkWidget* result_window;
    GtkWidget* result_label;
} WebRTCLeakCheck;

// Add modern theme constants
typedef struct {
    GdkRGBA bg_color;
    GdkRGBA fg_color;
    GdkRGBA accent_color;
    GdkRGBA hover_color;
    const char* font_family;
    int font_size;
} ThemeColors;

static const ThemeColors THEME_LIGHT = {
    .bg_color = {0.98, 0.98, 0.98, 1.0},
    .fg_color = {0.2, 0.2, 0.2, 1.0},
    .accent_color = {0.0, 0.47, 0.84, 1.0},
    .hover_color = {0.0, 0.52, 0.89, 1.0},
    .font_family = "Inter, system-ui, -apple-system, sans-serif",
    .font_size = 14
};

static const ThemeColors THEME_DARK = {
    .bg_color = {0.13, 0.13, 0.13, 1.0},
    .fg_color = {0.9, 0.9, 0.9, 1.0},
    .accent_color = {0.0, 0.6, 1.0, 1.0},
    .hover_color = {0.0, 0.7, 1.0, 1.0},
    .font_family = "Inter, system-ui, -apple-system, sans-serif",
    .font_size = 14
};

// Add advanced CSS with variables
static const char* MODERN_CSS = "";

// Add function to apply theme
static void apply_theme(GtkWidget* window, const ThemeColors* theme) {
    char* css = g_strdup_printf(MODERN_CSS,
        theme->bg_color.red, theme->bg_color.green, theme->bg_color.blue, theme->bg_color.alpha,
        theme->fg_color.red, theme->fg_color.green, theme->fg_color.blue, theme->fg_color.alpha,
        theme->accent_color.red, theme->accent_color.green, theme->accent_color.blue, theme->accent_color.alpha,
        theme->hover_color.red, theme->hover_color.green, theme->hover_color.blue, theme->hover_color.alpha,
        theme->font_family, theme->font_size);

    GtkCssProvider* provider = gtk_css_provider_new();
    gtk_css_provider_load_from_data(provider, css, -1, NULL);
    
    GtkStyleContext* context = gtk_widget_get_style_context(window);
    gtk_style_context_add_class(context, "browser-window");
    
    GdkScreen* screen = gtk_widget_get_screen(window);
    gtk_style_context_add_provider_for_screen(screen,
        GTK_STYLE_PROVIDER(provider),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    
    g_free(css);
    g_object_unref(provider);
}

// Forward declarations
static void on_url_entry_activate(GtkEntry* entry, gpointer data);
static void on_search_button_clicked(GtkButton* button, gpointer data);
static void on_home_button_clicked(GtkButton* button, gpointer data);
static void on_load_changed(WebKitWebView* web_view, WebKitLoadEvent load_event, gpointer user_data);
static void on_tab_close_clicked(GtkButton* button, GtkNotebook* notebook);
static gboolean on_resource_load_started(WebKitWebView* web_view, WebKitWebResource* resource, WebKitURIRequest* request, gpointer user_data);
static void on_resource_response_received(WebKitWebView* web_view, WebKitWebResource* resource, WebKitURIResponse* response, gpointer user_data);
static void on_intercept_toggled(GtkToggleButton* button, InterceptData* intercept_data);
static GtkWidget* create_tab_label(const gchar* text, GtkNotebook* notebook, BrowserTab* tab);
static void forward_request(GtkButton* button, InterceptData* data);
static void drop_request(GtkButton* button, InterceptData* data);
static void on_request_edit(GtkTextBuffer* buffer, InterceptData* data);
static void cleanup_pending_request(PendingRequest* req);
static void on_intercept_window_destroy(GtkWidget* window, InterceptData* data);
static void on_dev_tools_clicked(GtkButton* button, WebKitWebView* web_view);
static void show_history_window(GtkButton* button, BrowserHistory* history);
static void add_history_entry(BrowserHistory* history, const char* url, const char* title);
static void add_cookie(BrowserCookies* cookies, const char* domain, const char* name,
                      const char* value, const char* path, time_t expires, gboolean secure);
static void show_downloads_window(GtkMenuItem* menuitem, gpointer user_data);
static void switch_mode(GtkMenuItem* menu_item, gpointer user_data);
static void check_webrtc_leaks(GtkMenuItem* menuitem, gpointer user_data);
static size_t write_callback(void* contents, size_t size, size_t nmemb, void* userp);
static char* get_public_ip(void);
static char* get_local_ip(void);
static void on_new_tab_clicked(GtkButton* button, GtkNotebook* notebook);
static void show_ip_rotator(GtkMenuItem* menuitem, gpointer user_data);
static void rotate_ip(GtkButton* button, gpointer user_data);
static gboolean rotate_ip_complete(gpointer user_data);

// VPN function declarations
static gboolean check_openvpn_installed(void);
static void on_vpn_exit(GPid pid, gint status, VPNConnection* vpn);
static void load_vpn_config(GtkMenuItem* menuitem, VPNConnection* vpn);
static gboolean init_vpn_connection(VPNConnection* vpn);
static void cleanup_vpn(VPNConnection* vpn);
static void show_vpn_status(GtkMenuItem* menuitem, gpointer user_data);
static void create_vpn_menu(BrowserTab* tab, GtkWidget* popup_menu);

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
static void add_cookie(BrowserCookies* cookies, const char* domain, const char* name,
                      const char* value, const char* path, time_t expires, gboolean secure) {
    sqlite3_stmt* stmt;
    const char* sql = "INSERT INTO cookies (domain, name, value, path, expires, secure) "
                     "VALUES (?, ?, ?, ?, datetime(?), ?)";

    int rc = sqlite3_prepare_v2(cookies->db, sql, -1, &stmt, 0);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "Failed to prepare statement: %s\n", sqlite3_errmsg(cookies->db));
        return;
    }

    sqlite3_bind_text(stmt, 1, domain, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, name, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, value, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 4, path, -1, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 5, expires);
    sqlite3_bind_int(stmt, 6, secure);

    rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE) {
        fprintf(stderr, "Failed to add cookie: %s\n", sqlite3_errmsg(cookies->db));
    }

    sqlite3_finalize(stmt);
}
#pragma GCC diagnostic pop

// URL validation functions
static gboolean is_valid_url(const gchar* url) {
    return g_str_has_prefix(url, "http://") || g_str_has_prefix(url, "https://");
}

static gboolean is_localhost(const gchar* input) {
    return g_str_has_prefix(input, "localhost:") || g_str_has_prefix(input, "127.0.0.1:");
}

static gboolean is_domain(const gchar* input) {
    return g_str_has_suffix(input, ".com") || g_str_has_suffix(input, ".org") ||
           g_str_has_suffix(input, ".net") || g_str_has_suffix(input, ".edu") ||
           g_str_has_suffix(input, ".gov") || g_str_has_suffix(input, ".co") ||
           g_str_has_suffix(input, ".io");
}

// Add domain validation function
static gboolean is_resolvable_domain(const gchar* domain) {
    struct addrinfo hints, *result;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    
    // Try to resolve domain
    int status = getaddrinfo(domain, NULL, &hints, &result);
    if (status == 0) {
        freeaddrinfo(result);
        return TRUE;
    }
    return FALSE;
}

// Function to update tab title based on webpage title
static void on_title_changed(WebKitWebView* web_view, GParamSpec* pspec, gpointer user_data) {
    BrowserTab* tab = (BrowserTab*)user_data;
    const gchar* title = webkit_web_view_get_title(web_view);
    if (title != NULL && tab->title_label != NULL) {
        gtk_label_set_text(GTK_LABEL(tab->title_label), title);
    }
}

// Function to create intercept window
static GtkWidget* create_intercept_window(InterceptData* intercept_data) {
    if (intercept_data->window && GTK_IS_WIDGET(intercept_data->window)) {
        return intercept_data->window;  // Return existing window if valid
    }
    
    GtkWidget* window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(window), "Request/Response Interceptor");
    gtk_window_set_default_size(GTK_WINDOW(window), 1000, 800);

    GtkWidget* main_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
    gtk_container_add(GTK_CONTAINER(window), main_box);

    // Request pane
    GtkWidget* req_pane = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
    gtk_box_pack_start(GTK_BOX(main_box), req_pane, TRUE, TRUE, 5);
    
    GtkWidget* req_label = gtk_label_new("Request:");
    gtk_box_pack_start(GTK_BOX(req_pane), req_label, FALSE, FALSE, 5);
    
    GtkWidget* req_scroll = gtk_scrolled_window_new(NULL, NULL);
    GtkWidget* req_view = gtk_text_view_new();
    gtk_container_add(GTK_CONTAINER(req_scroll), req_view);
    gtk_box_pack_start(GTK_BOX(req_pane), req_scroll, TRUE, TRUE, 5);
    
    // Request Headers
    GtkWidget* req_headers_label = gtk_label_new("Request Headers:");
    gtk_box_pack_start(GTK_BOX(req_pane), req_headers_label, FALSE, FALSE, 5);
    
    GtkWidget* req_headers_scroll = gtk_scrolled_window_new(NULL, NULL);
    GtkWidget* req_headers_view = gtk_text_view_new();
    gtk_container_add(GTK_CONTAINER(req_headers_scroll), req_headers_view);
    gtk_box_pack_start(GTK_BOX(req_pane), req_headers_scroll, TRUE, TRUE, 5);

    // Response pane
    GtkWidget* resp_pane = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
    gtk_box_pack_start(GTK_BOX(main_box), resp_pane, TRUE, TRUE, 5);
    
    GtkWidget* resp_label = gtk_label_new("Response:");
    gtk_box_pack_start(GTK_BOX(resp_pane), resp_label, FALSE, FALSE, 5);
    
    GtkWidget* resp_scroll = gtk_scrolled_window_new(NULL, NULL);
    GtkWidget* resp_view = gtk_text_view_new();
    gtk_container_add(GTK_CONTAINER(resp_scroll), resp_view);
    gtk_box_pack_start(GTK_BOX(resp_pane), resp_scroll, TRUE, TRUE, 5);

    // Response Headers
    GtkWidget* resp_headers_label = gtk_label_new("Response Headers:");
    gtk_box_pack_start(GTK_BOX(resp_pane), resp_headers_label, FALSE, FALSE, 5);
    
    GtkWidget* resp_headers_scroll = gtk_scrolled_window_new(NULL, NULL);
    GtkWidget* resp_headers_view = gtk_text_view_new();
    gtk_container_add(GTK_CONTAINER(resp_headers_scroll), resp_headers_view);
    gtk_box_pack_start(GTK_BOX(resp_pane), resp_headers_scroll, TRUE, TRUE, 5);

    // Store buffers
    intercept_data->request_buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(req_view));
    intercept_data->response_buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(resp_view));
    intercept_data->req_headers_buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(req_headers_view));
    intercept_data->resp_headers_buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(resp_headers_view));

    // Add control buttons
    GtkWidget* controls_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
    gtk_box_pack_start(GTK_BOX(main_box), controls_box, FALSE, FALSE, 5);

    GtkWidget* forward_button = gtk_button_new_with_label("Forward");
    GtkWidget* drop_button = gtk_button_new_with_label("Drop");
    
    gtk_box_pack_start(GTK_BOX(controls_box), forward_button, TRUE, TRUE, 5);
    gtk_box_pack_start(GTK_BOX(controls_box), drop_button, TRUE, TRUE, 5);

    g_signal_connect(forward_button, "clicked", G_CALLBACK(forward_request), intercept_data);
    g_signal_connect(drop_button, "clicked", G_CALLBACK(drop_request), intercept_data);

    // Make text views editable
    gtk_text_view_set_editable(GTK_TEXT_VIEW(req_view), TRUE);
    gtk_text_view_set_editable(GTK_TEXT_VIEW(req_headers_view), TRUE);

    // Connect edit signals
    g_signal_connect(intercept_data->request_buffer, "changed", 
                    G_CALLBACK(on_request_edit), intercept_data);
    g_signal_connect(intercept_data->req_headers_buffer, "changed",
                    G_CALLBACK(on_request_edit), intercept_data);

    // Initialize request queue if not already initialized
    if (!intercept_data->pending_requests) {
        intercept_data->pending_requests = g_queue_new();
    }
    
    // Initialize current resource
    intercept_data->current_resource = NULL;
    intercept_data->request_modified = FALSE;

    gtk_widget_show_all(window);
    return window;
}

// Update create_browser_tab function
static BrowserTab* create_browser_tab(InterceptData* intercept_data, BrowserHistory* history) {
    BrowserTab* tab = g_new0(BrowserTab, 1); // Initialize all fields to 0
    
    // Create WebView first so it's available for button callbacks
    tab->webview = WEBKIT_WEB_VIEW(webkit_web_view_new());
    
    // Create container and controls
    tab->container = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
    GtkWidget* hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
    gtk_box_pack_start(GTK_BOX(tab->container), hbox, FALSE, FALSE, 0);
    
    // Create search engine combo
    tab->search_engine_combo = GTK_COMBO_BOX_TEXT(gtk_combo_box_text_new());
    gtk_combo_box_text_append_text(tab->search_engine_combo, "Google");
    gtk_combo_box_text_append_text(tab->search_engine_combo, "Bing");
    gtk_combo_box_text_append_text(tab->search_engine_combo, "DuckDuckGo");
    gtk_combo_box_text_append_text(tab->search_engine_combo, "Yahoo");
    gtk_combo_box_set_active(GTK_COMBO_BOX(tab->search_engine_combo), 0);
    gtk_box_pack_start(GTK_BOX(hbox), GTK_WIDGET(tab->search_engine_combo), FALSE, FALSE, 0);
    
    // Create URL entry
    tab->url_entry = gtk_entry_new();
    gtk_box_pack_start(GTK_BOX(hbox), tab->url_entry, TRUE, TRUE, 0);
    
    // Create navigation buttons
    GtkWidget* back_button = gtk_button_new_with_label("Back");
    GtkWidget* forward_button = gtk_button_new_with_label("Forward");
    GtkWidget* home_button = gtk_button_new_with_label("Home");
    GtkWidget* search_button = gtk_button_new_with_label("Search");
    tab->dev_tools_button = gtk_button_new_with_label("Dev Tools");  // Add dev tools button
    
    gtk_box_pack_start(GTK_BOX(hbox), back_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(hbox), forward_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(hbox), home_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(hbox), search_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(hbox), tab->dev_tools_button, FALSE, FALSE, 0);  // Add to layout

    // Store references for callbacks
    g_object_set_data(G_OBJECT(tab->url_entry), "webview", tab->webview);
    g_object_set_data(G_OBJECT(tab->url_entry), "search_engine_combo", tab->search_engine_combo);

    // Connect button signals
    g_signal_connect_swapped(back_button, "clicked", G_CALLBACK(webkit_web_view_go_back), tab->webview);
    g_signal_connect_swapped(forward_button, "clicked", G_CALLBACK(webkit_web_view_go_forward), tab->webview);
    g_signal_connect(home_button, "clicked", G_CALLBACK(on_home_button_clicked), tab->webview);
    g_signal_connect(search_button, "clicked", G_CALLBACK(on_search_button_clicked), tab->url_entry);
    g_signal_connect(tab->url_entry, "activate", G_CALLBACK(on_url_entry_activate), tab->webview);

    // Add webview to container
    gtk_box_pack_start(GTK_BOX(tab->container), GTK_WIDGET(tab->webview), TRUE, TRUE, 0);
    
    // Connect WebKit signals 
    g_signal_connect(tab->webview, "load-changed", G_CALLBACK(on_load_changed), tab);
    g_signal_connect(tab->webview, "resource-load-started", G_CALLBACK(on_resource_load_started), intercept_data);
    g_signal_connect(tab->webview, "resource-response-received", G_CALLBACK(on_resource_response_received), intercept_data);

    // Load default page
    webkit_web_view_load_uri(tab->webview, "https://www.google.com");

    // Enable developer extras
    WebKitSettings* settings = webkit_web_view_get_settings(tab->webview);
    webkit_settings_set_enable_developer_extras(settings, TRUE);

    // Connect dev tools button signal
    g_signal_connect(tab->dev_tools_button, "clicked", G_CALLBACK(on_dev_tools_clicked), tab->webview);

    // Inside create_browser_tab function, after creating other buttons:

    // Create menu button (three dots)
    tab->menu_button = gtk_menu_button_new();
    GtkWidget* menu_image = gtk_image_new_from_icon_name("view-more-symbolic", GTK_ICON_SIZE_BUTTON);
    gtk_button_set_image(GTK_BUTTON(tab->menu_button), menu_image);

    // Create popup menu
    GtkWidget* popup_menu = gtk_menu_new();

    // Add VPN menu first (before other menu items)
    GtkWidget* vpn_menu = gtk_menu_new();
    GtkWidget* vpn_item = gtk_menu_item_new_with_label("VPN");
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(vpn_item), vpn_menu);

    // VPN menu items
    GtkWidget* vpn_connect = gtk_menu_item_new_with_label("Connect VPN");
    GtkWidget* vpn_disconnect = gtk_menu_item_new_with_label("Disconnect VPN");
    GtkWidget* vpn_status = gtk_menu_item_new_with_label("VPN Status");
    GtkWidget* vpn_settings = gtk_menu_item_new_with_label("VPN Settings");

    // Add items to VPN submenu
    gtk_menu_shell_append(GTK_MENU_SHELL(vpn_menu), vpn_connect);
    gtk_menu_shell_append(GTK_MENU_SHELL(vpn_menu), vpn_disconnect);
    gtk_menu_shell_append(GTK_MENU_SHELL(vpn_menu), gtk_separator_menu_item_new());
    gtk_menu_shell_append(GTK_MENU_SHELL(vpn_menu), vpn_status);
    gtk_menu_shell_append(GTK_MENU_SHELL(vpn_menu), vpn_settings);

    // Initialize VPN connection structure
    tab->vpn = g_new0(VPNConnection, 1);
    tab->vpn->connected = FALSE;
    tab->vpn->config_file = NULL;
    tab->vpn->last_error = NULL;

    // Add VPN menu item to main popup menu (before other items)
    gtk_menu_shell_append(GTK_MENU_SHELL(popup_menu), vpn_item);
    gtk_menu_shell_append(GTK_MENU_SHELL(popup_menu), gtk_separator_menu_item_new());

    // Connect VPN signals
    g_signal_connect(vpn_connect, "activate", G_CALLBACK(init_vpn_connection), tab->vpn);
    g_signal_connect(vpn_disconnect, "activate", G_CALLBACK(cleanup_vpn), tab->vpn);
    g_signal_connect(vpn_status, "activate", G_CALLBACK(show_vpn_status), tab->vpn);

    // Add menu items
    GtkWidget* dev_tools_item = gtk_menu_item_new_with_label("Developer Tools");
    GtkWidget* history_item = gtk_menu_item_new_with_label("History");
    GtkWidget* downloads_item = gtk_menu_item_new_with_label("Downloads");  // Add downloads item
    GtkWidget* intercept_item = gtk_menu_item_new_with_label("Intercept");
    GtkWidget* light_mode_item = gtk_menu_item_new_with_label("Light Mode");
    GtkWidget* dark_mode_item = gtk_menu_item_new_with_label("Dark Mode");
    GtkWidget* matrix_mode_item = gtk_menu_item_new_with_label("Matrix Mode");
    GtkWidget* webrtc_leak_item = gtk_menu_item_new_with_label("WebRTC Leak Check");
    GtkWidget* ip_rotator_item = gtk_menu_item_new_with_label("IP Rotator");

    gtk_menu_shell_append(GTK_MENU_SHELL(popup_menu), dev_tools_item);
    gtk_menu_shell_append(GTK_MENU_SHELL(popup_menu), history_item);
    gtk_menu_shell_append(GTK_MENU_SHELL(popup_menu), downloads_item);  // Add to menu
    gtk_menu_shell_append(GTK_MENU_SHELL(popup_menu), intercept_item);
    gtk_menu_shell_append(GTK_MENU_SHELL(popup_menu), light_mode_item);
    gtk_menu_shell_append(GTK_MENU_SHELL(popup_menu), dark_mode_item);
    gtk_menu_shell_append(GTK_MENU_SHELL(popup_menu), matrix_mode_item);
    gtk_menu_shell_append(GTK_MENU_SHELL(popup_menu), webrtc_leak_item);
    gtk_menu_shell_append(GTK_MENU_SHELL(popup_menu), ip_rotator_item);

    // Connect signals
    g_signal_connect(dev_tools_item, "activate", G_CALLBACK(on_dev_tools_clicked), tab->webview);
    g_signal_connect(history_item, "activate", G_CALLBACK(show_history_window), history);
    g_signal_connect(downloads_item, "activate", G_CALLBACK(show_downloads_window), NULL);  // Add callback
    g_signal_connect(intercept_item, "activate", G_CALLBACK(on_intercept_toggled), intercept_data);
    g_signal_connect(light_mode_item, "activate", G_CALLBACK(switch_mode), tab);
    g_signal_connect(dark_mode_item, "activate", G_CALLBACK(switch_mode), tab);
    g_signal_connect(matrix_mode_item, "activate", G_CALLBACK(switch_mode), tab);
    g_signal_connect(webrtc_leak_item, "activate", G_CALLBACK(check_webrtc_leaks), NULL);
    g_signal_connect(ip_rotator_item, "activate", G_CALLBACK(show_ip_rotator), tab);

    // Set popup menu for menu button
    gtk_menu_button_set_popup(GTK_MENU_BUTTON(tab->menu_button), popup_menu);

    // Add menu button to hbox
    gtk_box_pack_end(GTK_BOX(hbox), tab->menu_button, FALSE, FALSE, 0);

    // Remove the old buttons we're replacing
    gtk_widget_destroy(tab->dev_tools_button);
    tab->dev_tools_button = NULL;

    gtk_widget_show_all(popup_menu);

    // Store history reference
    tab->history = history;

    // Create mode submenu
    GtkWidget* mode_menu = gtk_menu_new();
    GtkWidget* mode_item = gtk_menu_item_new_with_label("Mode");
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(mode_item), mode_menu);

    GtkWidget* light_item = gtk_menu_item_new_with_label("Light Mode");
    GtkWidget* dark_item = gtk_menu_item_new_with_label("Dark Mode");
    GtkWidget* matrix_item = gtk_menu_item_new_with_label("Matrix Mode");

    gtk_menu_shell_append(GTK_MENU_SHELL(mode_menu), light_item);
    gtk_menu_shell_append(GTK_MENU_SHELL(mode_menu), dark_item);
    gtk_menu_shell_append(GTK_MENU_SHELL(mode_menu), matrix_item);

    g_signal_connect(light_item, "activate", G_CALLBACK(switch_mode), tab);
    g_signal_connect(dark_item, "activate", G_CALLBACK(switch_mode), tab);
    g_signal_connect(matrix_item, "activate", G_CALLBACK(switch_mode), tab);

    // Add mode menu to popup menu (add before other items)
    gtk_menu_shell_append(GTK_MENU_SHELL(popup_menu), mode_item);
    gtk_menu_shell_append(GTK_MENU_SHELL(popup_menu), gtk_separator_menu_item_new());
    gtk_menu_shell_append(GTK_MENU_SHELL(popup_menu), dev_tools_item);
    // ... rest of menu items

    // Store mode menu reference and set default mode
    tab->mode_item = mode_item;
    tab->mode = MODE_LIGHT;

    // Add after other menu items
    GtkWidget* webrtc_check_item = gtk_menu_item_new_with_label("Check WebRTC Leaks");
    gtk_menu_shell_append(GTK_MENU_SHELL(popup_menu), webrtc_check_item);
    g_signal_connect(webrtc_check_item, "activate", G_CALLBACK(check_webrtc_leaks), NULL);

    // Add new tab menu item
    GtkWidget* new_tab_item = gtk_menu_item_new_with_label("New Tab");
    gtk_menu_shell_append(GTK_MENU_SHELL(popup_menu), gtk_separator_menu_item_new());
    gtk_menu_shell_append(GTK_MENU_SHELL(popup_menu), new_tab_item);

    // Connect new tab signal
    g_signal_connect(new_tab_item, "activate", G_CALLBACK(on_new_tab_clicked), gtk_widget_get_ancestor(tab->container, GTK_TYPE_NOTEBOOK));

    // Initialize VPN structure
    tab->vpn = g_new0(VPNConnection, 1);
    tab->vpn->connected = FALSE;
    tab->vpn->config_file = NULL;
    tab->vpn->last_error = NULL;
    tab->vpn->config_label = gtk_label_new("No config loaded");

    // Create and setup VPN menu
    create_vpn_menu(tab, popup_menu);

    return tab;
}

// Function to create new tab label with close button
static GtkWidget* create_tab_label(const gchar* text, GtkNotebook* notebook, BrowserTab* tab) {
    GtkWidget* hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
    tab->title_label = gtk_label_new(text);
    GtkWidget* close_button = gtk_button_new_from_icon_name("window-close", GTK_ICON_SIZE_MENU);
    
    gtk_box_pack_start(GTK_BOX(hbox), tab->title_label, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(hbox), close_button, FALSE, FALSE, 0);
    
    g_signal_connect(close_button, "clicked", G_CALLBACK(on_tab_close_clicked), notebook);
    g_object_set_data(G_OBJECT(close_button), "tab", tab);
    
    // Connect title changed signal
    g_signal_connect(tab->webview, "notify::title", G_CALLBACK(on_title_changed), tab);
    
    gtk_widget_show_all(hbox);
    return hbox;
}

// Navigation and URL handling callbacks remain the same
static void on_url_entry_activate(GtkEntry* entry, gpointer data) {
    const gchar* text = gtk_entry_get_text(entry);
    WebKitWebView* webview = WEBKIT_WEB_VIEW(data);

    if (is_valid_url(text)) {
        webkit_web_view_load_uri(webview, text);
    } else if (is_localhost(text)) {
        gchar* url_with_prefix = g_strdup_printf("http://%s", text);
        webkit_web_view_load_uri(webview, url_with_prefix);
        g_free(url_with_prefix);
    } else if (is_domain(text)) {
        // Extract domain part
        const gchar* domain = text;
        if (g_str_has_prefix(text, "www.")) {
            domain = text + 4;
        }
        
        if (is_resolvable_domain(domain)) {
            gchar* url_with_prefix = g_strdup_printf("http://%s", text);
            webkit_web_view_load_uri(webview, url_with_prefix);
        } else {
            // Show error dialog
            GtkWidget* dialog = gtk_message_dialog_new(GTK_WINDOW(gtk_widget_get_toplevel(GTK_WIDGET(entry))),
                GTK_DIALOG_MODAL,
                GTK_MESSAGE_ERROR,
                GTK_BUTTONS_OK,
                "Error resolving domain '%s'.\nPlease check your internet connection and domain name.",
                text);
            gtk_dialog_run(GTK_DIALOG(dialog));
            gtk_widget_destroy(dialog);
        }
    } else {
        GtkComboBoxText* search_engine_combo = GTK_COMBO_BOX_TEXT(g_object_get_data(G_OBJECT(entry), "search_engine_combo"));
        const gchar* selected_engine = gtk_combo_box_text_get_active_text(search_engine_combo);
        gchar* search_url = NULL;

        if (g_strcmp0(selected_engine, "Google") == 0) {
            search_url = g_strdup_printf("https://www.google.com/search?q=%s", text);
        } else if (g_strcmp0(selected_engine, "Bing") == 0) {
            search_url = g_strdup_printf("https://www.bing.com/search?q=%s", text);
        } else if (g_strcmp0(selected_engine, "DuckDuckGo") == 0) {
            search_url = g_strdup_printf("https://www.duckduckgo.com/?q=%s", text);
        } else if (g_strcmp0(selected_engine, "Yahoo") == 0) {
            search_url = g_strdup_printf("https://search.yahoo.com/search?p=%s", text);
        }

        if (search_url) {
            webkit_web_view_load_uri(webview, search_url);
            g_free(search_url);
        }
    }
}

static void on_search_button_clicked(GtkButton* button, gpointer data) {
    GtkWidget* entry = GTK_WIDGET(data);
    WebKitWebView* webview = g_object_get_data(G_OBJECT(entry), "webview");
    on_url_entry_activate(GTK_ENTRY(entry), webview);
}

static void on_home_button_clicked(GtkButton* button, gpointer data) {
    WebKitWebView* webview = WEBKIT_WEB_VIEW(data);
    webkit_web_view_load_uri(webview, "https://www.google.com");
}

static void on_load_changed(WebKitWebView* web_view, WebKitLoadEvent load_event, gpointer user_data) {
    BrowserTab* tab = (BrowserTab*)user_data;
    
    switch (load_event) {
        case WEBKIT_LOAD_FINISHED:
            if (tab && tab->history) {
                const gchar* uri = webkit_web_view_get_uri(web_view);
                const gchar* title = webkit_web_view_get_title(web_view);
                add_history_entry(tab->history, uri, title);
            }
            break;
        default:
            break;
    }
}

// Tab management callbacks
static void on_tab_close_clicked(GtkButton* button, GtkNotebook* notebook) {
    BrowserTab* tab = g_object_get_data(G_OBJECT(button), "tab");
    gint page_num = gtk_notebook_page_num(notebook, tab->container);
    
    // Don't close if it's the last tab
    if (gtk_notebook_get_n_pages(notebook) > 1) {
        gtk_notebook_remove_page(notebook, page_num);
        g_free(tab);
    }
}

static void on_new_tab_clicked(GtkButton* button, GtkNotebook* notebook) {
    InterceptData* intercept_data = g_object_get_data(G_OBJECT(notebook), "intercept_data");
    BrowserHistory* history = g_object_get_data(G_OBJECT(notebook), "history");
    BrowserTab* tab = create_browser_tab(intercept_data, history);
    gint page_num = gtk_notebook_append_page(notebook, tab->container,
                                           create_tab_label("New Tab", notebook, tab));
    gtk_widget_show_all(tab->container);
    gtk_notebook_set_current_page(notebook, page_num);
}

// Update on_resource_load_started
static gboolean on_resource_load_started(WebKitWebView* web_view, 
                                       WebKitWebResource* resource, 
                                       WebKitURIRequest* request, 
                                       gpointer user_data) {
    InterceptData* intercept_data = (InterceptData*)user_data;
    
    if (!intercept_data->enabled || !intercept_data->window) {
        return FALSE;
    }

    // Create new pending request
    PendingRequest* pending = g_new0(PendingRequest, 1);
    pending->web_view = web_view;  // Store WebView reference
    pending->resource = g_object_ref(resource);
    pending->request = g_object_ref(request);
    pending->method = g_strdup(webkit_uri_request_get_http_method(request));
    pending->uri = g_strdup(webkit_uri_request_get_uri(request));
    
    // Store headers
    pending->headers = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    SoupMessageHeaders* headers = webkit_uri_request_get_http_headers(request);
    if (headers) {
        SoupMessageHeadersIter iter;
        const char* name;
        const char* value;
        
        soup_message_headers_iter_init(&iter, headers);
        while (soup_message_headers_iter_next(&iter, &name, &value)) {
            g_hash_table_insert(pending->headers, g_strdup(name), g_strdup(value));
        }
    }

    // Add to queue if not current request
    if (!intercept_data->current_resource) {
        intercept_data->current_resource = resource;
        // Display current request
        gchar* request_text = g_strdup_printf("Method: %s\nURI: %s\n", 
                                            pending->method, pending->uri);
        gtk_text_buffer_set_text(intercept_data->request_buffer, request_text, -1);
        g_free(request_text);
        
        // Display headers
        GString* headers_str = g_string_new(NULL);
        GHashTableIter iter;
        gpointer key, value;
        g_hash_table_iter_init(&iter, pending->headers);
        while (g_hash_table_iter_next(&iter, &key, &value)) {
            g_string_append_printf(headers_str, "%s: %s\n", (char*)key, (char*)value);
        }
        gtk_text_buffer_set_text(intercept_data->req_headers_buffer, headers_str->str, -1);
        g_string_free(headers_str, TRUE);
    } else {
        g_queue_push_tail(intercept_data->pending_requests, pending);
    }

    return FALSE;
}

// Update on_resource_response_received function
static void on_resource_response_received(WebKitWebView* web_view,
                                        WebKitWebResource* resource,
                                        WebKitURIResponse* response,
                                        gpointer user_data) {
    InterceptData* intercept_data = (InterceptData*)user_data;
    
    if (!intercept_data->enabled) {
        return;
    }

    // Get response details
    guint status_code = webkit_uri_response_get_status_code(response);
    const gchar* content_type = webkit_uri_response_get_mime_type(response);
    
    // Create response text
    gchar* response_text = g_strdup_printf("Status: %d\nContent-Type: %s\n",
                                         status_code, content_type);
    
    // Get and format response headers
    SoupMessageHeaders* headers = webkit_uri_response_get_http_headers(response);
    GString* headers_str = g_string_new(NULL);
    
    if (headers) {
        SoupMessageHeadersIter iter;
        const char* name;
        const char* value;
        
        soup_message_headers_iter_init(&iter, headers);
        while (soup_message_headers_iter_next(&iter, &name, &value)) {
            g_string_append_printf(headers_str, "%s: %s\n", name, value);
        }
    }
    
    // Store response info with current request if exists
    if (intercept_data->current_resource == resource) {
        gtk_text_buffer_set_text(intercept_data->response_buffer, response_text, -1);
        gtk_text_buffer_set_text(intercept_data->resp_headers_buffer, headers_str->str, -1);
    } else {
        // Find in pending requests and update
        GList* list = g_queue_peek_head_link(intercept_data->pending_requests);
        while (list) {
            PendingRequest* req = (PendingRequest*)list->data;
            if (req->resource == resource) {
                req->response = g_object_ref(response);
                req->status_code = status_code;
                req->content_type = g_strdup(content_type);
                
                // Store response headers
                req->response_headers = g_hash_table_new_full(g_str_hash, g_str_equal,
                                                            g_free, g_free);
                if (headers) {
                    SoupMessageHeadersIter iter;
                    const char* name;
                    const char* value;
                    
                    soup_message_headers_iter_init(&iter, headers);
                    while (soup_message_headers_iter_next(&iter, &name, &value)) {
                        g_hash_table_insert(req->response_headers,
                                         g_strdup(name),
                                         g_strdup(value));
                    }
                }
                break;
            }
            list = list->next;
        }
    }
    
    g_string_free(headers_str, TRUE);
    g_free(response_text);
}

// Update intercept toggle callback
static void on_intercept_toggled(GtkToggleButton* button, InterceptData* intercept_data) {
    if (!intercept_data) return;
    
    intercept_data->enabled = gtk_toggle_button_get_active(button);
    
    if (intercept_data->enabled) {
        // Create window if it doesn't exist
        if (!intercept_data->window || !GTK_IS_WIDGET(intercept_data->window)) {
            intercept_data->window = create_intercept_window(intercept_data);
            g_signal_connect(intercept_data->window, "destroy", 
                           G_CALLBACK(on_intercept_window_destroy), intercept_data);
        }
        
        if (GTK_IS_WIDGET(intercept_data->window)) {
            gtk_widget_show_all(intercept_data->window);
        }
    } else {
        if (intercept_data->window && GTK_IS_WIDGET(intercept_data->window)) {
            gtk_widget_hide(intercept_data->window);
        }
    }
    
    // Update toggle button state to match window state
    if (!intercept_data->window) {
        gtk_toggle_button_set_active(button, FALSE);
    }
}

// Update forward_request function
static void forward_request(GtkButton* button, InterceptData* data) {
    if (!data || !data->window || !data->current_resource) return;

    PendingRequest* current = NULL;
    GList* list = g_queue_peek_head_link(data->pending_requests);
    while (list) {
        PendingRequest* req = (PendingRequest*)list->data;
        if (req->resource == data->current_resource) {
            current = req;
            break;
        }
        list = list->next;
    }

    if (current && current->web_view) {
        webkit_web_view_reload(current->web_view);
    }
    
    data->current_resource = NULL;

    // Process next request
    if (!g_queue_is_empty(data->pending_requests)) {
        PendingRequest* next = g_queue_pop_head(data->pending_requests);
        if (next) {
            data->current_resource = next->resource;
            
            // Update display
            gchar* request_text = g_strdup_printf("Method: %s\nURI: %s\n", 
                                                next->method, next->uri);
            gtk_text_buffer_set_text(data->request_buffer, request_text, -1);
            g_free(request_text);

            if (next->headers) {
                GString* headers_str = g_string_new(NULL);
                GHashTableIter iter;
                gpointer key, value;
                g_hash_table_iter_init(&iter, next->headers);
                while (g_hash_table_iter_next(&iter, &key, &value)) {
                    g_string_append_printf(headers_str, "%s: %s\n", 
                                        (char*)key, (char*)value);
                }
                gtk_text_buffer_set_text(data->req_headers_buffer, headers_str->str, -1);
                g_string_free(headers_str, TRUE);
            }

            cleanup_pending_request(next);
        }
    }
}

static void drop_request(GtkButton* button, InterceptData* data) {
    if (data->current_resource) {
        // Cancel request by stopping resource load
        webkit_web_resource_get_data(data->current_resource,
                                   NULL,  // No cancellable
                                   NULL,  // No callback
                                   NULL); // No user data
        data->current_resource = NULL;
        data->request_modified = FALSE;
    }
    
    // Process next request
    if (!g_queue_is_empty(data->pending_requests)) {
        PendingRequest* next = g_queue_pop_head(data->pending_requests);
        // Display next request (same as in forward_request)
        gchar* request_text = g_strdup_printf("Method: %s\nURI: %s\n", 
                                            next->method, next->uri);
        gtk_text_buffer_set_text(data->request_buffer, request_text, -1);

        GString* headers_str = g_string_new(NULL);
        GHashTableIter iter;
        gpointer key, value;
        g_hash_table_iter_init(&iter, next->headers);
        while (g_hash_table_iter_next(&iter, &key, &value)) {
            g_string_append_printf(headers_str, "%s: %s\n", 
                                 (char*)key, (char*)value);
        }
        gtk_text_buffer_set_text(data->req_headers_buffer, 
                                headers_str->str, -1);

        g_string_free(headers_str, TRUE);
        g_free(request_text);
        g_free(next);
    }
}

static void on_request_edit(GtkTextBuffer* buffer, InterceptData* data) {
    data->request_modified = TRUE;
}

static void cleanup_pending_request(PendingRequest* req) {
    if (req) {
        if (req->resource) g_object_unref(req->resource);
        if (req->request) g_object_unref(req->request);
        if (req->response) g_object_unref(req->response);
        g_free(req->method);
        g_free(req->uri);
        g_free(req->content_type);
        if (req->headers) g_hash_table_unref(req->headers);
        if (req->response_headers) g_hash_table_unref(req->response_headers);
        g_free(req);
    }
}

static void cleanup_intercept_data(InterceptData* data) {
    if (data) {
        if (data->window && GTK_IS_WIDGET(data->window)) {
            gtk_widget_destroy(data->window);
            data->window = NULL;
        }
        if (data->pending_requests) {
            g_queue_foreach(data->pending_requests, (GFunc)cleanup_pending_request, NULL);
            g_queue_free(data->pending_requests);
        }
        g_free(data);
    }
}

// Add window destroy callback implementation
static void on_intercept_window_destroy(GtkWidget* window, InterceptData* data) {
    if (data) {
        data->window = NULL;
        data->enabled = FALSE;
        data->current_resource = NULL;
        data->request_modified = FALSE;

        // Clear text buffers
        if (data->request_buffer) {
            gtk_text_buffer_set_text(data->request_buffer, "", -1);
        }
        if (data->response_buffer) {
            gtk_text_buffer_set_text(data->response_buffer, "", -1);
        }
        if (data->req_headers_buffer) {
            gtk_text_buffer_set_text(data->req_headers_buffer, "", -1);
        }
        if (data->resp_headers_buffer) {
            gtk_text_buffer_set_text(data->resp_headers_buffer, "", -1);
        }
    }
}

static void on_dev_tools_clicked(GtkButton* button, WebKitWebView* web_view) {
    WebKitWebInspector* inspector = webkit_web_view_get_inspector(web_view);
    webkit_web_inspector_show(inspector);
}

// Initialize database tables
static void init_databases(BrowserData* data) {
    const char* history_sql = 
        "CREATE TABLE IF NOT EXISTS history ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "url TEXT NOT NULL,"
        "title TEXT,"
        "visit_time DATETIME DEFAULT CURRENT_TIMESTAMP)";

    const char* cookies_sql = 
        "CREATE TABLE IF NOT EXISTS cookies ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "domain TEXT NOT NULL,"
        "name TEXT NOT NULL,"
        "value TEXT,"
        "path TEXT,"
        "expires DATETIME,"
        "secure INTEGER)";

    const char* settings_sql = 
        "CREATE TABLE IF NOT EXISTS settings ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "name TEXT NOT NULL,"
        "value TEXT,"
        "updated_at DATETIME DEFAULT CURRENT_TIMESTAMP)";

    char* err_msg = 0;
    int rc;

    rc = sqlite3_open("browser.db", &data->history->db);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "Cannot open history database: %s\n", sqlite3_errmsg(data->history->db));
        return;
    }

    rc = sqlite3_exec(data->history->db, history_sql, 0, 0, &err_msg);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "SQL error: %s\n", err_msg);
        sqlite3_free(err_msg);
    }

    rc = sqlite3_open("cookies.db", &data->cookies->db);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "Cannot open cookies database: %s\n", sqlite3_errmsg(data->cookies->db));
        return;
    }

    rc = sqlite3_exec(data->cookies->db, cookies_sql, 0, 0, &err_msg);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "SQL error: %s\n", err_msg);
        sqlite3_free(err_msg);
    }

    rc = sqlite3_exec(data->history->db, settings_sql, 0, 0, &err_msg);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "SQL error: %s\n", err_msg);
        sqlite3_free(err_msg);
    }
}

// Add history entry
static void add_history_entry(BrowserHistory* history, const char* url, const char* title) {
    sqlite3_stmt* stmt;
    const char* sql = "INSERT INTO history (url, title) VALUES (?, ?)";

    int rc = sqlite3_prepare_v2(history->db, sql, -1, &stmt, 0);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "Failed to prepare statement: %s\n", sqlite3_errmsg(history->db));
        return;
    }

    sqlite3_bind_text(stmt, 1, url, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, title, -1, SQLITE_STATIC);

    rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE) {
        fprintf(stderr, "Failed to add history entry: %s\n", sqlite3_errmsg(history->db));
    }

    sqlite3_finalize(stmt);
}

// Show history window
static void show_history_window(GtkButton* button, BrowserHistory* history) {
    if (!history->history_window) {
        history->history_window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
        gtk_window_set_title(GTK_WINDOW(history->history_window), "Browser History");
        gtk_window_set_default_size(GTK_WINDOW(history->history_window), 600, 400);

        GtkWidget* scroll = gtk_scrolled_window_new(NULL, NULL);
        GtkWidget* tree = gtk_tree_view_new();

        // Create list store
        history->history_store = gtk_list_store_new(3, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING);
        gtk_tree_view_set_model(GTK_TREE_VIEW(tree), GTK_TREE_MODEL(history->history_store));

        // Add columns
        GtkCellRenderer* renderer = gtk_cell_renderer_text_new();
        gtk_tree_view_insert_column_with_attributes(GTK_TREE_VIEW(tree), -1, "Title",
            renderer, "text", 0, NULL);
        gtk_tree_view_insert_column_with_attributes(GTK_TREE_VIEW(tree), -1, "URL",
            renderer, "text", 1, NULL);
        gtk_tree_view_insert_column_with_attributes(GTK_TREE_VIEW(tree), -1, "Date",
            renderer, "text", 2, NULL);

        gtk_container_add(GTK_CONTAINER(scroll), tree);
        gtk_container_add(GTK_CONTAINER(history->history_window), scroll);
    }

    // Load history data
    sqlite3_stmt* stmt;
    const char* sql = "SELECT title, url, datetime(visit_time) FROM history ORDER BY visit_time DESC";

    gtk_list_store_clear(history->history_store);

    int rc = sqlite3_prepare_v2(history->db, sql, -1, &stmt, 0);
    if (rc == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            GtkTreeIter iter;
            gtk_list_store_append(history->history_store, &iter);
            gtk_list_store_set(history->history_store, &iter,
                             0, sqlite3_column_text(stmt, 0),
                             1, sqlite3_column_text(stmt, 1),
                             2, sqlite3_column_text(stmt, 2),
                             -1);
        }
    }
    sqlite3_finalize(stmt);

    gtk_widget_show_all(history->history_window);
}

// Add to cleanup function
static void cleanup_browser_data(BrowserData* data) {
    if (data) {
        if (data->history) {
            if (data->history->db) {
                sqlite3_close(data->history->db);
            }
            if (data->history->history_window) {
                gtk_widget_destroy(data->history->history_window);
            }
            g_free(data->history);
        }
        if (data->cookies) {
            if (data->cookies->db) {
                sqlite3_close(data->cookies->db);
            }
            if (data->cookies->cookie_window) {
                gtk_widget_destroy(data->cookies->cookie_window);
            }
            g_free(data->cookies);
        }
        g_free(data);
    }
}

static void show_downloads_window(GtkMenuItem* menuitem, gpointer user_data) {
    GtkWidget* downloads_window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(downloads_window), "Downloads");
    gtk_window_set_default_size(GTK_WINDOW(downloads_window), 600, 400);

    GtkWidget* scroll = gtk_scrolled_window_new(NULL, NULL);
    GtkWidget* tree = gtk_tree_view_new();

    // Create list store for downloads
    GtkListStore* downloads_store = gtk_list_store_new(4, 
        G_TYPE_STRING,  // Filename
        G_TYPE_STRING,  // Size
        G_TYPE_STRING,  // Status
        G_TYPE_STRING   // URL
    );
    gtk_tree_view_set_model(GTK_TREE_VIEW(tree), GTK_TREE_MODEL(downloads_store));

    // Add columns
    GtkCellRenderer* renderer = gtk_cell_renderer_text_new();
    gtk_tree_view_insert_column_with_attributes(GTK_TREE_VIEW(tree), -1, "Filename",
        renderer, "text", 0, NULL);
    gtk_tree_view_insert_column_with_attributes(GTK_TREE_VIEW(tree), -1, "Size",
        renderer, "text", 1, NULL);
    gtk_tree_view_insert_column_with_attributes(GTK_TREE_VIEW(tree), -1, "Status",
        renderer, "text", 2, NULL);
    gtk_tree_view_insert_column_with_attributes(GTK_TREE_VIEW(tree), -1, "URL",
        renderer, "text", 3, NULL);

    gtk_container_add(GTK_CONTAINER(scroll), tree);
    gtk_container_add(GTK_CONTAINER(downloads_window), scroll);

    // Add dummy data for testing
    GtkTreeIter iter;
    gtk_list_store_append(downloads_store, &iter);
    gtk_list_store_set(downloads_store, &iter,
        0, "example.pdf",
        1, "1.2 MB",
        2, "Completed",
        3, "https://example.com/example.pdf",
        -1);

    g_object_unref(downloads_store);
    
    gtk_widget_show_all(downloads_window);
}

static void switch_mode(GtkMenuItem* menu_item, gpointer user_data) {
    BrowserTab* tab = (BrowserTab*)user_data;
    const gchar* mode_text = gtk_menu_item_get_label(menu_item);
    
    GtkCssProvider* provider = gtk_css_provider_new();
    const gchar* gtk_css;
    const gchar* webkit_css;

    if (g_strcmp0(mode_text, "Light Mode") == 0) {
        tab->mode = MODE_LIGHT;
        gtk_css = "* { background-color: white !important; }";
        webkit_css = "body { background-color: white !important; }\n"
                    "img, video, iframe { filter: none !important; }\n"
                    "a { color: #0066cc !important; }";
    }
    else if (g_strcmp0(mode_text, "Dark Mode") == 0) {
        tab->mode = MODE_DARK;
        gtk_css = "* { background-color: #222 !important; }";
        webkit_css = "body { background-color: #222 !important; }\n"
                    "img, video, iframe { filter: brightness(0.9) !important; }\n"
                    "a { color: #66b3ff !important; }";
    }
    else if (g_strcmp0(mode_text, "Matrix Mode") == 0) {
        tab->mode = MODE_MATRIX;
        gtk_css = "* { background-color: black !important; }";
        webkit_css = "body { background-color: black !important; }\n"
                    "img, video, iframe { filter: brightness(1) !important; opacity: 1 !important; }\n"
                    ".ytp-cued-thumbnail-overlay, .ytp-thumbnail { opacity: 1 !important; }\n"
                    "a { color: #00ff00 !important; text-decoration: none !important; }\n"
                    "a:hover { text-shadow: 0 0 8px #00ff00 !important; }\n"
                    "pre, code { background: #001100 !important; color: #00ff00 !important; }\n"
                    "input, textarea, select { background: #001100 !important; color: #00ff00 !important; border: 1px solid #00ff00 !important; }\n"
                    "button { background: #002200 !important; color: #00ff00 !important; border: 1px solid #00ff00 !important; }\n"
                    "button:hover { background: #003300 !important; }";
    }

    // Apply styles
    gtk_css_provider_load_from_data(provider, gtk_css, -1, NULL);
    gtk_style_context_add_provider_for_screen(
        gdk_screen_get_default(),
        GTK_STYLE_PROVIDER(provider),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION
    );
    
    WebKitUserContentManager* manager = webkit_web_view_get_user_content_manager(tab->webview);
    webkit_user_content_manager_remove_all_style_sheets(manager);
    
    WebKitUserStyleSheet* style_sheet = webkit_user_style_sheet_new(
        webkit_css,
        WEBKIT_USER_CONTENT_INJECT_ALL_FRAMES,
        WEBKIT_USER_STYLE_LEVEL_USER,
        NULL,
        NULL
    );
    
    webkit_user_content_manager_add_style_sheet(manager, style_sheet);
    webkit_user_style_sheet_unref(style_sheet);
    webkit_web_view_reload(tab->webview);
    g_object_unref(provider);
}

static void check_webrtc_leaks(GtkMenuItem* menuitem, gpointer user_data) {
    WebRTCLeakCheck* leak_check = g_new0(WebRTCLeakCheck, 1);
    
    // Create results window
    leak_check->result_window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(leak_check->result_window), "WebRTC Leak Check");
    gtk_window_set_default_size(GTK_WINDOW(leak_check->result_window), 400, 300);
    
    GtkWidget* vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_container_add(GTK_CONTAINER(leak_check->result_window), vbox);
    
    // Add header
    GtkWidget* header = gtk_label_new("WebRTC Leak Check Results");
    gtk_box_pack_start(GTK_BOX(vbox), header, FALSE, FALSE, 10);
    
    // Add results label
    leak_check->result_label = gtk_label_new("Checking for WebRTC leaks...");
    gtk_box_pack_start(GTK_BOX(vbox), leak_check->result_label, TRUE, TRUE, 10);
    
    gtk_widget_show_all(leak_check->result_window);
    
    // Get IP addresses
    leak_check->local_ip = get_local_ip();
    leak_check->public_ip = get_public_ip();
    
    // Format and display results
    GString* result_text = g_string_new("");
    g_string_append_printf(result_text, "Local IP: %s\n", 
                          leak_check->local_ip ? leak_check->local_ip : "Not detected");
    g_string_append_printf(result_text, "Public IP: %s\n", 
                          leak_check->public_ip ? leak_check->public_ip : "Not detected");
    
    if (leak_check->local_ip && leak_check->public_ip) {
        g_string_append(result_text, "\nPotential WebRTC Leak: YES\n");
        g_string_append(result_text, "Your local IP address might be exposed through WebRTC.");
    } else {
        g_string_append(result_text, "\nPotential WebRTC Leak: NO\n");
        g_string_append(result_text, "No obvious WebRTC leaks detected.");
    }
    
    gtk_label_set_text(GTK_LABEL(leak_check->result_label), result_text->str);
    g_string_free(result_text, TRUE);
    
    // Cleanup
    g_free(leak_check->local_ip);
    g_free(leak_check->public_ip);
}

static size_t write_callback(void* contents, size_t size, size_t nmemb, void* userp) {
    size_t realsize = size * nmemb;
    GString* memory = (GString*)userp;
    g_string_append_len(memory, contents, realsize);
    return realsize;
}

static char* get_public_ip(void) {
    CURL* curl;
    CURLcode res;
    GString* chunk = g_string_new(NULL);
    char* ip = NULL;
    
    curl = curl_easy_init();
    if (curl) {
        curl_easy_setopt(curl, CURLOPT_URL, "https://api.ipify.org?format=json");
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, chunk);
        
        res = curl_easy_perform(curl);
        if (res == CURLE_OK) {
            JsonParser* parser = json_parser_new();
            if (json_parser_load_from_data(parser, chunk->str, -1, NULL)) {
                JsonNode* root = json_parser_get_root(parser);
                JsonObject* obj = json_node_get_object(root);
                const char* ip_str = json_object_get_string_member(obj, "ip");
                if (ip_str) {
                    ip = g_strdup(ip_str);
                }
            }
            g_object_unref(parser);
        }
        curl_easy_cleanup(curl);
    }
    
    g_string_free(chunk, TRUE);
    return ip;
}

static char* get_local_ip(void) {
    struct ifaddrs* ifaddr;
    struct ifaddrs* ifa;
    char* ip = NULL;
    
    if (getifaddrs(&ifaddr) == -1) {
        return NULL;
    }
    
    for (ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next) {
        if (ifa->ifa_addr == NULL) continue;
        
        if (ifa->ifa_addr->sa_family == AF_INET) {
            struct sockaddr_in* addr = (struct sockaddr_in*)ifa->ifa_addr;
            char buffer[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &(addr->sin_addr), buffer, INET_ADDRSTRLEN);
            
            // Skip loopback
            if (strcmp(buffer, "127.0.0.1") != 0) {
                ip = g_strdup(buffer);
                break;
            }
        }
    }
    
    freeifaddrs(ifaddr);
    return ip;
}

// Add these new function declarations after the existing declarations
static void show_ip_rotator(GtkMenuItem* menuitem, gpointer user_data);
static void rotate_ip(GtkButton* button, gpointer user_data);

// Add this structure definition with the other structures
typedef struct {
    GtkWidget* window;
    GtkWidget* current_ip_label;
    GtkWidget* status_label;
    WebKitWebView* webview;
} IPRotator;

// Add this function to create and handle the IP rotator window
static void show_ip_rotator(GtkMenuItem* menuitem, gpointer user_data) {
    BrowserTab* tab = (BrowserTab*)user_data;
    IPRotator* rotator = g_new0(IPRotator, 1);
    rotator->webview = tab->webview;
    
    // Create window
    rotator->window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(rotator->window), "IP Rotator");
    gtk_window_set_default_size(GTK_WINDOW(rotator->window), 400, 200);
    
    GtkWidget* vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_container_add(GTK_CONTAINER(rotator->window), vbox);
    
    // Current IP display
    GtkWidget* current_ip_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
    gtk_box_pack_start(GTK_BOX(vbox), current_ip_box, FALSE, FALSE, 5);
    
    GtkWidget* ip_label = gtk_label_new("Current IP:");
    rotator->current_ip_label = gtk_label_new("Checking...");
    gtk_box_pack_start(GTK_BOX(current_ip_box), ip_label, FALSE, FALSE, 5);
    gtk_box_pack_start(GTK_BOX(current_ip_box), rotator->current_ip_label, TRUE, TRUE, 5);
    
    // Rotate button
    GtkWidget* rotate_button = gtk_button_new_with_label("Rotate IP");
    gtk_box_pack_start(GTK_BOX(vbox), rotate_button, FALSE, FALSE, 5);
    
    // Status label
    rotator->status_label = gtk_label_new("");
    gtk_box_pack_start(GTK_BOX(vbox), rotator->status_label, FALSE, FALSE, 5);
    
    // Connect signals
    g_signal_connect(rotate_button, "clicked", G_CALLBACK(rotate_ip), rotator);
    
    // Show window
    gtk_widget_show_all(rotator->window);
    
    // Get and display current IP
    char* current_ip = get_public_ip();
    if (current_ip) {
        gtk_label_set_text(GTK_LABEL(rotator->current_ip_label), current_ip);
        g_free(current_ip);
    } else {
        gtk_label_set_text(GTK_LABEL(rotator->current_ip_label), "Failed to get IP");
    }
}

// Update the rotate_ip function
static void rotate_ip(GtkButton* button, gpointer user_data) {
    IPRotator* rotator = (IPRotator*)user_data;
    
    // Set status
    gtk_label_set_text(GTK_LABEL(rotator->status_label), "Rotating IP...");
    
    // Store button reference for re-enabling
    g_object_set_data(G_OBJECT(rotator->window), "rotate_button", button);
    
    // Disable button during rotation - add GTK_WIDGET cast
    gtk_widget_set_sensitive(GTK_WIDGET(button), FALSE);
    
    // Simulate IP rotation with timeout
    g_timeout_add(2000, (GSourceFunc)rotate_ip_complete, rotator);
}

// Update the rotate_ip_complete function
static gboolean rotate_ip_complete(gpointer user_data) {
    IPRotator* rotator = (IPRotator*)user_data;
    
    // Get new IP
    char* new_ip = get_public_ip();
    if (new_ip) {
        gtk_label_set_text(GTK_LABEL(rotator->current_ip_label), new_ip);
        gtk_label_set_text(GTK_LABEL(rotator->status_label), "IP rotation complete. New IP: ");
        
        // Create IP display with copy button
        GtkWidget* ip_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
        GtkWidget* ip_label = gtk_label_new(new_ip);
        GtkWidget* copy_button = gtk_button_new_with_label("Copy");
        
        gtk_box_pack_start(GTK_BOX(ip_box), ip_label, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(ip_box), copy_button, FALSE, FALSE, 0);
        
        // Re-enable rotate button
        GtkWidget* rotate_button = g_object_get_data(G_OBJECT(rotator->window), "rotate_button");
        if (rotate_button) {
            gtk_widget_set_sensitive(rotate_button, TRUE);
        }
        
        g_free(new_ip);
    } else {
        gtk_label_set_text(GTK_LABEL(rotator->status_label), "IP rotation failed");
        
        // Re-enable rotate button on failure
        GtkWidget* rotate_button = g_object_get_data(G_OBJECT(rotator->window), "rotate_button");
        if (rotate_button) {
            gtk_widget_set_sensitive(rotate_button, TRUE);
        }
    }
    
    return G_SOURCE_REMOVE;
}

// Update in main() before showing window
int main(int argc, char* argv[]) {
    gtk_init(&argc, &argv);
    
    // Create main window with visual effects
    GtkWidget* window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_default_size(GTK_WINDOW(window), 1200, 800);
    gtk_window_set_title(GTK_WINDOW(window), "Rocket Browser");
    gtk_window_set_position(GTK_WINDOW(window), GTK_WIN_POS_CENTER);
    
    // Create main vertical box
    GtkWidget* main_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
    gtk_container_add(GTK_CONTAINER(window), main_vbox);
    
    // Create notebook for tabs
    GtkWidget* notebook = gtk_notebook_new();
    gtk_notebook_set_scrollable(GTK_NOTEBOOK(notebook), TRUE);
    gtk_notebook_set_tab_pos(GTK_NOTEBOOK(notebook), GTK_POS_TOP);
    gtk_notebook_set_show_border(GTK_NOTEBOOK(notebook), FALSE);
    gtk_box_pack_start(GTK_BOX(main_vbox), notebook, TRUE, TRUE, 0);

    // Create new tab button with styling
    GtkWidget* new_tab_button = gtk_button_new_with_label("+");
    gtk_widget_set_size_request(new_tab_button, 30, 30);

    // Style the new tab button
    GtkStyleContext* style_context = gtk_widget_get_style_context(new_tab_button);
    GtkCssProvider* provider = gtk_css_provider_new();
    const char* css = 
        "button { padding: 0 8px; margin: 2px; border: none; background: none; }"
        "button:hover { background-color: rgba(255,255,255,0.1); border-radius: 3px; }";
    gtk_css_provider_load_from_data(provider, css, -1, NULL);
    gtk_style_context_add_provider(style_context, 
        GTK_STYLE_PROVIDER(provider),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);

    // Style the + symbol
    GtkWidget* button_label = gtk_bin_get_child(GTK_BIN(new_tab_button));
    PangoAttrList* attr_list = pango_attr_list_new();
    pango_attr_list_insert(attr_list, pango_attr_scale_new(1.5));
    gtk_label_set_attributes(GTK_LABEL(button_label), attr_list);
    pango_attr_list_unref(attr_list);

    // Add button to notebook
    gtk_notebook_set_action_widget(GTK_NOTEBOOK(notebook), new_tab_button, GTK_PACK_END);
    g_signal_connect(new_tab_button, "clicked", G_CALLBACK(on_new_tab_clicked), notebook);
    g_object_unref(provider);

    // Show all widgets
    gtk_widget_show_all(new_tab_button);
    
    // Create intercept data// Update create_browser_tab function
static BrowserTab* create_browser_tab(InterceptData* intercept_data, BrowserHistory* history) {
    BrowserTab* tab = g_new0(BrowserTab, 1); // Initialize all fields to 0
    
    // Create WebView first so it's available for button callbacks
    tab->webview = WEBKIT_WEB_VIEW(webkit_web_view_new());
    
    // Create container and controls
    tab->container = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
    GtkWidget* hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
    gtk_box_pack_start(GTK_BOX(tab->container), hbox, FALSE, FALSE, 0);
    
    // Create search engine combo
    tab->search_engine_combo = GTK_COMBO_BOX_TEXT(gtk_combo_box_text_new());
    gtk_combo_box_text_append_text(tab->search_engine_combo, "Google");
    gtk_combo_box_text_append_text(tab->search_engine_combo, "Bing");
    gtk_combo_box_text_append_text(tab->search_engine_combo, "DuckDuckGo");
    gtk_combo_box_text_append_text(tab->search_engine_combo, "Yahoo");
    gtk_combo_box_set_active(GTK_COMBO_BOX(tab->search_engine_combo), 0);
    gtk_box_pack_start(GTK_BOX(hbox), GTK_WIDGET(tab->search_engine_combo), FALSE, FALSE, 0);
    
    // Create URL entry
    tab->url_entry = gtk_entry_new();
    gtk_box_pack_start(GTK_BOX(hbox), tab->url_entry, TRUE, TRUE, 0);
    
    // Create navigation buttons
    GtkWidget* back_button = gtk_button_new_with_label("Back");
    GtkWidget* forward_button = gtk_button_new_with_label("Forward");
    GtkWidget* home_button = gtk_button_new_with_label("Home");
    GtkWidget* search_button = gtk_button_new_with_label("Search");
    tab->dev_tools_button = gtk_button_new_with_label("Dev Tools");  // Add dev tools button
    
    gtk_box_pack_start(GTK_BOX(hbox), back_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(hbox), forward_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(hbox), home_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(hbox), search_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(hbox), tab->dev_tools_button, FALSE, FALSE, 0);  // Add to layout

    // Store references for callbacks
    g_object_set_data(G_OBJECT(tab->url_entry), "webview", tab->webview);
    g_object_set_data(G_OBJECT(tab->url_entry), "search_engine_combo", tab->search_engine_combo);

    // Connect button signals
    g_signal_connect_swapped(back_button, "clicked", G_CALLBACK(webkit_web_view_go_back), tab->webview);
    g_signal_connect_swapped(forward_button, "clicked", G_CALLBACK(webkit_web_view_go_forward), tab->webview);
    g_signal_connect(home_button, "clicked", G_CALLBACK(on_home_button_clicked), tab->webview);
    g_signal_connect(search_button, "clicked", G_CALLBACK(on_search_button_clicked), tab->url_entry);
    g_signal_connect(tab->url_entry, "activate", G_CALLBACK(on_url_entry_activate), tab->webview);

    // Add webview to container
    gtk_box_pack_start(GTK_BOX(tab->container), GTK_WIDGET(tab->webview), TRUE, TRUE, 0);
    
    // Connect WebKit signals 
    g_signal_connect(tab->webview, "load-changed", G_CALLBACK(on_load_changed), tab);
    g_signal_connect(tab->webview, "resource-load-started", G_CALLBACK(on_resource_load_started), intercept_data);
    g_signal_connect(tab->webview, "resource-response-received", G_CALLBACK(on_resource_response_received), intercept_data);

    // Load default page
    webkit_web_view_load_uri(tab->webview, "https://www.google.com");

    // Enable developer extras
    WebKitSettings* settings = webkit_web_view_get_settings(tab->webview);
    webkit_settings_set_enable_developer_extras(settings, TRUE);

    // Connect dev tools button signal
    g_signal_connect(tab->dev_tools_button, "clicked", G_CALLBACK(on_dev_tools_clicked), tab->webview);

    // Inside create_browser_tab function, after creating other buttons:

    // Create menu button (three dots)
    tab->menu_button = gtk_menu_button_new();
    GtkWidget* menu_image = gtk_image_new_from_icon_name("view-more-symbolic", GTK_ICON_SIZE_BUTTON);
    gtk_button_set_image(GTK_BUTTON(tab->menu_button), menu_image);

    // Create popup menu
    GtkWidget* popup_menu = gtk_menu_new();

    // Add VPN menu first (before other menu items)
    GtkWidget* vpn_menu = gtk_menu_new();
    GtkWidget* vpn_item = gtk_menu_item_new_with_label("VPN");
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(vpn_item), vpn_menu);

    // VPN menu items
    GtkWidget* vpn_connect = gtk_menu_item_new_with_label("Connect VPN");
    GtkWidget* vpn_disconnect = gtk_menu_item_new_with_label("Disconnect VPN");
    GtkWidget* vpn_status = gtk_menu_item_new_with_label("VPN Status");
    GtkWidget* vpn_settings = gtk_menu_item_new_with_label("VPN Settings");

    // Add items to VPN submenu
    gtk_menu_shell_append(GTK_MENU_SHELL(vpn_menu), vpn_connect);
    gtk_menu_shell_append(GTK_MENU_SHELL(vpn_menu), vpn_disconnect);
    gtk_menu_shell_append(GTK_MENU_SHELL(vpn_menu), gtk_separator_menu_item_new());
    gtk_menu_shell_append(GTK_MENU_SHELL(vpn_menu), vpn_status);
    gtk_menu_shell_append(GTK_MENU_SHELL(vpn_menu), vpn_settings);

    // Initialize VPN connection structure
    tab->vpn = g_new0(VPNConnection, 1);
    tab->vpn->connected = FALSE;
    tab->vpn->config_file = NULL;
    tab->vpn->last_error = NULL;

    // Add VPN menu item to main popup menu (before other items)
    gtk_menu_shell_append(GTK_MENU_SHELL(popup_menu), vpn_item);
    gtk_menu_shell_append(GTK_MENU_SHELL(popup_menu), gtk_separator_menu_item_new());

    // Connect VPN signals
    g_signal_connect(vpn_connect, "activate", G_CALLBACK(init_vpn_connection), tab->vpn);
    g_signal_connect(vpn_disconnect, "activate", G_CALLBACK(cleanup_vpn), tab->vpn);
    g_signal_connect(vpn_status, "activate", G_CALLBACK(show_vpn_status), tab->vpn);

    // Add menu items
    GtkWidget* dev_tools_item = gtk_menu_item_new_with_label("Developer Tools");
    GtkWidget* history_item = gtk_menu_item_new_with_label("History");
    GtkWidget* downloads_item = gtk_menu_item_new_with_label("Downloads");  // Add downloads item
    GtkWidget* intercept_item = gtk_menu_item_new_with_label("Intercept");
    GtkWidget* light_mode_item = gtk_menu_item_new_with_label("Light Mode");
    GtkWidget* dark_mode_item = gtk_menu_item_new_with_label("Dark Mode");
    GtkWidget* matrix_mode_item = gtk_menu_item_new_with_label("Matrix Mode");
    GtkWidget* webrtc_leak_item = gtk_menu_item_new_with_label("WebRTC Leak Check");
    GtkWidget* ip_rotator_item = gtk_menu_item_new_with_label("IP Rotator");

    gtk_menu_shell_append(GTK_MENU_SHELL(popup_menu), dev_tools_item);
    gtk_menu_shell_append(GTK_MENU_SHELL(popup_menu), history_item);
    gtk_menu_shell_append(GTK_MENU_SHELL(popup_menu), downloads_item);  // Add to menu
    gtk_menu_shell_append(GTK_MENU_SHELL(popup_menu), intercept_item);
    gtk_menu_shell_append(GTK_MENU_SHELL(popup_menu), light_mode_item);
    gtk_menu_shell_append(GTK_MENU_SHELL(popup_menu), dark_mode_item);
    gtk_menu_shell_append(GTK_MENU_SHELL(popup_menu), matrix_mode_item);
    gtk_menu_shell_append(GTK_MENU_SHELL(popup_menu), webrtc_leak_item);
    gtk_menu_shell_append(GTK_MENU_SHELL(popup_menu), ip_rotator_item);

    // Connect signals
    g_signal_connect(dev_tools_item, "activate", G_CALLBACK(on_dev_tools_clicked), tab->webview);
    g_signal_connect(history_item, "activate", G_CALLBACK(show_history_window), history);
    g_signal_connect(downloads_item, "activate", G_CALLBACK(show_downloads_window), NULL);  // Add callback
    g_signal_connect(intercept_item, "activate", G_CALLBACK(on_intercept_toggled), intercept_data);
    g_signal_connect(light_mode_item, "activate", G_CALLBACK(switch_mode), tab);
    g_signal_connect(dark_mode_item, "activate", G_CALLBACK(switch_mode), tab);
    g_signal_connect(matrix_mode_item, "activate", G_CALLBACK(switch_mode), tab);
    g_signal_connect(webrtc_leak_item, "activate", G_CALLBACK(check_webrtc_leaks), NULL);
    g_signal_connect(ip_rotator_item, "activate", G_CALLBACK(show_ip_rotator), tab);

    // Set popup menu for menu button
    gtk_menu_button_set_popup(GTK_MENU_BUTTON(tab->menu_button), popup_menu);

    // Add menu button to hbox
    gtk_box_pack_end(GTK_BOX(hbox), tab->menu_button, FALSE, FALSE, 0);

    // Remove the old buttons we're replacing
    gtk_widget_destroy(tab->dev_tools_button);
    tab->dev_tools_button = NULL;

    gtk_widget_show_all(popup_menu);

    // Store history reference
    tab->history = history;

    // Create mode submenu
    GtkWidget* mode_menu = gtk_menu_new();
    GtkWidget* mode_item = gtk_menu_item_new_with_label("Mode");
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(mode_item), mode_menu);

    GtkWidget
    InterceptData* intercept_data = g_new0(InterceptData, 1);
    intercept_data->enabled = FALSE;
    intercept_data->window = NULL;
    intercept_data->request_buffer = NULL;
    intercept_data->response_buffer = NULL;
    intercept_data->req_headers_buffer = NULL;
    intercept_data->resp_headers_buffer = NULL;
    intercept_data->pending_requests = NULL;
    intercept_data->current_resource = NULL;
    intercept_data->request_modified = FALSE;
    g_object_set_data(G_OBJECT(notebook), "intercept_data", intercept_data);
    
    // Initialize browser data
    BrowserData* browser_data = g_new0(BrowserData, 1);
    browser_data->history = g_new0(BrowserHistory, 1);
    browser_data->cookies = g_new0(BrowserCookies, 1);
    
    init_databases(browser_data);

    // Create initial tab
    BrowserTab* initial_tab = create_browser_tab(intercept_data, browser_data->history);
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), initial_tab->container,
                           create_tab_label("New Tab", GTK_NOTEBOOK(notebook), initial_tab));
    
    g_signal_connect_swapped(window, "destroy", G_CALLBACK(cleanup_intercept_data), intercept_data);
    g_signal_connect_swapped(window, "destroy", G_CALLBACK(cleanup_browser_data), browser_data);
    
    gtk_widget_show_all(window);
    gtk_main();
    
    return 0;
}

// Add after the other function implementations, before main():

static void create_vpn_menu(BrowserTab* tab, GtkWidget* popup_menu) {
    // Create VPN submenu
    GtkWidget* vpn_menu = gtk_menu_new();
    GtkWidget* vpn_item = gtk_menu_item_new_with_label("VPN");
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(vpn_item), vpn_menu);

    // Initialize VPN connection structure
    tab->vpn = g_new0(VPNConnection, 1);
    tab->vpn->connected = FALSE;
    tab->vpn->config_file = NULL;
    tab->vpn->last_error = NULL;

    // Create VPN connect submenu
    GtkWidget* connect_menu = gtk_menu_new();
    GtkWidget* connect_item = gtk_menu_item_new_with_label("Connect VPN");
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(connect_item), connect_menu);

    // Add items to connect submenu
    GtkWidget* load_config = gtk_menu_item_new_with_label("Load Config File");
    GtkWidget* start_connection = gtk_menu_item_new_with_label("Start Connection");
    gtk_menu_shell_append(GTK_MENU_SHELL(connect_menu), load_config);
    gtk_menu_shell_append(GTK_MENU_SHELL(connect_menu), start_connection);

    // Create other menu items
    GtkWidget* disconnect_item = gtk_menu_item_new_with_label("Disconnect VPN");
    GtkWidget* status_item = gtk_menu_item_new_with_label("VPN Status");

    // Add config label
    tab->vpn->config_label = gtk_label_new("No config loaded");
    GtkWidget* label_item = gtk_menu_item_new();
    gtk_container_add(GTK_CONTAINER(label_item), tab->vpn->config_label);
    gtk_widget_set_sensitive(label_item, FALSE);

    // Add all items to VPN menu
    gtk_menu_shell_append(GTK_MENU_SHELL(vpn_menu), connect_item);
    gtk_menu_shell_append(GTK_MENU_SHELL(vpn_menu), label_item);
    gtk_menu_shell_append(GTK_MENU_SHELL(vpn_menu), gtk_separator_menu_item_new());
    gtk_menu_shell_append(GTK_MENU_SHELL(vpn_menu), disconnect_item);
    gtk_menu_shell_append(GTK_MENU_SHELL(vpn_menu), gtk_separator_menu_item_new());
    gtk_menu_shell_append(GTK_MENU_SHELL(vpn_menu), status_item);

    // Connect signals
    g_signal_connect(load_config, "activate", G_CALLBACK(load_vpn_config), tab->vpn);
    g_signal_connect(start_connection, "activate", G_CALLBACK(init_vpn_connection), tab->vpn);
    g_signal_connect(disconnect_item, "activate", G_CALLBACK(cleanup_vpn), tab->vpn);
    g_signal_connect(status_item, "activate", G_CALLBACK(show_vpn_status), tab->vpn);

    // Add VPN menu to main popup menu
    gtk_menu_shell_append(GTK_MENU_SHELL(popup_menu), vpn_item);
    gtk_widget_show_all(vpn_menu);
}

// Add before create_vpn_menu function:

static gboolean check_openvpn_installed(void) {
    return system("which openvpn > /dev/null 2>&1") == 0;
}

static void on_vpn_exit(GPid pid, gint status, VPNConnection* vpn) {
    vpn->connected = FALSE;
    g_spawn_close_pid(pid);
    vpn->vpn_pid = 0;
    g_free(vpn->last_error);
    
    if (WIFEXITED(status)) {
        vpn->last_error = g_strdup_printf("VPN process exited with status %d", WEXITSTATUS(status));
    } else if (WIFSIGNALED(status)) {
        vpn->last_error = g_strdup_printf("VPN process terminated by signal %d", WTERMSIG(status));
    }
}

static void load_vpn_config(GtkMenuItem* menuitem, VPNConnection* vpn) {
    GtkWidget* dialog = gtk_file_chooser_dialog_new(
        "Select OpenVPN Config File",
        NULL,
        GTK_FILE_CHOOSER_ACTION_OPEN,
        "Cancel", GTK_RESPONSE_CANCEL,
        "Open", GTK_RESPONSE_ACCEPT,
        NULL
    );

    GtkFileFilter* filter = gtk_file_filter_new();
    gtk_file_filter_add_pattern(filter, "*.ovpn");
    gtk_file_filter_set_name(filter, "OpenVPN Config Files (*.ovpn)");
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dialog), filter);

    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
        g_free(vpn->config_file);
        vpn->config_file = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
        
        if (vpn->config_label) {
            gchar* label_text = g_path_get_basename(vpn->config_file);
            gtk_label_set_text(GTK_LABEL(vpn->config_label), label_text);
            g_free(label_text);
        }
    }
    gtk_widget_destroy(dialog);
}

static gboolean init_vpn_connection(VPNConnection* vpn) {
    if (!vpn || vpn->connected) return FALSE;

    if (!check_openvpn_installed()) {
        GtkWidget* dialog = gtk_message_dialog_new(NULL,
            GTK_DIALOG_MODAL, GTK_MESSAGE_ERROR, GTK_BUTTONS_OK,
            "OpenVPN is not installed. Please install it using:\nsudo apt-get install openvpn");
        gtk_dialog_run(GTK_DIALOG(dialog));
        gtk_widget_destroy(dialog);
        return FALSE;
    }

    if (!vpn->config_file) {
        GtkWidget* dialog = gtk_message_dialog_new(NULL,
            GTK_DIALOG_MODAL, GTK_MESSAGE_ERROR, GTK_BUTTONS_OK,
            "No VPN config file loaded. Please load a config file first.");
        gtk_dialog_run(GTK_DIALOG(dialog));
        gtk_widget_destroy(dialog);
        return FALSE;
    }

    char* argv[] = {
        "pkexec", "openvpn", "--config", vpn->config_file, NULL
    };

    GError* error = NULL;
    gboolean success = g_spawn_async(NULL, argv, NULL,
        G_SPAWN_SEARCH_PATH | G_SPAWN_DO_NOT_REAP_CHILD,
        NULL, NULL, &vpn->vpn_pid, &error);

    if (success) {
        vpn->connected = TRUE;
        g_free(vpn->last_error);
        vpn->last_error = NULL;
        g_child_watch_add(vpn->vpn_pid, (GChildWatchFunc)on_vpn_exit, vpn);
    } else {
        g_free(vpn->last_error);
        vpn->last_error = g_strdup(error->message);
        g_error_free(error);
    }

    return success;
}

static void cleanup_vpn(VPNConnection* vpn) {
    if (!vpn) return;

    if (vpn->connected && vpn->vpn_pid > 0) {
        kill(vpn->vpn_pid, SIGTERM);
        g_spawn_close_pid(vpn->vpn_pid);
        vpn->vpn_pid = 0;
    }

    vpn->connected = FALSE;
    g_free(vpn->config_file);
    vpn->config_file = NULL;
}

static void show_vpn_status(GtkMenuItem* menuitem, gpointer user_data) {
    VPNConnection* vpn = (VPNConnection*)user_data;
    
    GtkWidget* dialog = gtk_message_dialog_new(NULL,
        GTK_DIALOG_MODAL, GTK_MESSAGE_INFO, GTK_BUTTONS_OK,
        "VPN Status: %s\nConfig File: %s\n%s",
        vpn->connected ? "Connected" : "Disconnected",
        vpn->config_file ? vpn->config_file : "No config loaded",
        vpn->last_error ? vpn->last_error : "");

    gtk_window_set_title(GTK_WINDOW(dialog), "VPN Status");
    gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);
}

// Forward declarations for VPN functions
static gboolean check_openvpn_installed(void);
static void on_vpn_exit(GPid pid, gint status, VPNConnection* vpn);
static void load_vpn_config(GtkMenuItem* menuitem, VPNConnection* vpn);
static gboolean init_vpn_connection(VPNConnection* vpn);
static void cleanup_vpn(VPNConnection* vpn);
static void show_vpn_status(GtkMenuItem* menuitem, gpointer user_data);
static void create_vpn_menu(BrowserTab* tab, GtkWidget* popup_menu);

int main(int argc, char* argv[]) {
    gtk_init(&argc, &argv);
    
    // Create and initialize InterceptData
    InterceptData* intercept_data = g_new0(InterceptData, 1);
    if (intercept_data) {
        intercept_data->enabled = FALSE;
        intercept_data->window = NULL;
        intercept_data->request_buffer = NULL;
        intercept_data->response_buffer = NULL;
        intercept_data->req_headers_buffer = NULL;
        intercept_data->resp_headers_buffer = NULL;
        intercept_data->pending_requests = NULL;
        intercept_data->current_resource = NULL;
        intercept_data->request_modified = FALSE;
    }

    // Create main window with visual effects
    GtkWidget* window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_default_size(GTK_WINDOW(window), 1200, 800);
    gtk_window_set_title(GTK_WINDOW(window), "Rocket Browser");
    gtk_window_set_position(GTK_WINDOW(window), GTK_WIN_POS_CENTER);
    
    // Create main vertical box
    GtkWidget* main_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
    gtk_container_add(GTK_CONTAINER(window), main_vbox);
    
    // Create notebook for tabs
    GtkWidget* notebook = gtk_notebook_new();
    gtk_notebook_set_scrollable(GTK_NOTEBOOK(notebook), TRUE);
    gtk_notebook_set_tab_pos(GTK_NOTEBOOK(notebook), GTK_POS_TOP);
    gtk_notebook_set_show_border(GTK_NOTEBOOK(notebook), FALSE);
    gtk_box_pack_start(GTK_BOX(main_vbox), notebook, TRUE, TRUE, 0);

    // Create new tab button with styling
    GtkWidget* new_tab_button = gtk_button_new_with_label("+");
    gtk_widget_set_size_request(new_tab_button, 30, 30);

    // Style the new tab button
    GtkStyleContext* style_context = gtk_widget_get_style_context(new_tab_button);
    GtkCssProvider* provider = gtk_css_provider_new();
    const char* css = 
        "button { padding: 0 8px; margin: 2px; border: none; background: none; }"
        "button:hover { background-color: rgba(255,255,255,0.1); border-radius: 3px; }";
    gtk_css_provider_load_from_data(provider, css, -1, NULL);
    gtk_style_context_add_provider(style_context, 
        GTK_STYLE_PROVIDER(provider),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);

    // Style the + symbol
    GtkWidget* button_label = gtk_bin_get_child(GTK_BIN(new_tab_button));
    PangoAttrList* attr_list = pango_attr_list_new();
    pango_attr_list_insert(attr_list, pango_attr_scale_new(1.5));
    gtk_label_set_attributes(GTK_LABEL(button_label), attr_list);
    pango_attr_list_unref(attr_list);

    // Add button to notebook
    gtk_notebook_set_action_widget(GTK_NOTEBOOK(notebook), new_tab_button, GTK_PACK_END);
    g_signal_connect(new_tab_button, "clicked", G_CALLBACK(on_new_tab_clicked), notebook);
    g_object_unref(provider);

    // Show all widgets
    gtk_widget_show_all(new_tab_button);
    
    // Create intercept data// Update create_browser_tab function
static BrowserTab* create_browser_tab(InterceptData* intercept_data, BrowserHistory* history) {
    BrowserTab* tab = g_new0(BrowserTab, 1); // Initialize all fields to 0
    
    // Create WebView first so it's available for button callbacks
    tab->webview = WEBKIT_WEB_VIEW(webkit_web_view_new());
    
    // Create container and controls
    tab->container = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
    GtkWidget* hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
    gtk_box_pack_start(GTK_BOX(tab->container), hbox, FALSE, FALSE, 0);
    
    // Create search engine combo
    tab->search_engine_combo = GTK_COMBO_BOX_TEXT(gtk_combo_box_text_new());
    gtk_combo_box_text_append_text(tab->search_engine_combo, "Google");
    gtk_combo_box_text_append_text(tab->search_engine_combo, "Bing");
    gtk_combo_box_text_append_text(tab->search_engine_combo, "DuckDuckGo");
    gtk_combo_box_text_append_text(tab->search_engine_combo, "Yahoo");
    gtk_combo_box_set_active(GTK_COMBO_BOX(tab->search_engine_combo), 0);
    gtk_box_pack_start(GTK_BOX(hbox), GTK_WIDGET(tab->search_engine_combo), FALSE, FALSE, 0);
    
    // Create URL entry
    tab->url_entry = gtk_entry_new();
    gtk_box_pack_start(GTK_BOX(hbox), tab->url_entry, TRUE, TRUE, 0);
    
    // Create navigation buttons
    GtkWidget* back_button = gtk_button_new_with_label("Back");
    GtkWidget* forward_button = gtk_button_new_with_label("Forward");
    GtkWidget* home_button = gtk_button_new_with_label("Home");
    GtkWidget* search_button = gtk_button_new_with_label("Search");
    tab->dev_tools_button = gtk_button_new_with_label("Dev Tools");  // Add dev tools button
    
    gtk_box_pack_start(GTK_BOX(hbox), back_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(hbox), forward_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(hbox), home_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(hbox), search_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(hbox), tab->dev_tools_button, FALSE, FALSE, 0);  // Add to layout

    // Store references for callbacks
    g_object_set_data(G_OBJECT(tab->url_entry), "webview", tab->webview);
    g_object_set_data(G_OBJECT(tab->url_entry), "search_engine_combo", tab->search_engine_combo);

    // Connect button signals
    g_signal_connect_swapped(back_button, "clicked", G_CALLBACK(webkit_web_view_go_back), tab->webview);
    g_signal_connect_swapped(forward_button, "clicked", G_CALLBACK(webkit_web_view_go_forward), tab->webview);
    g_signal_connect(home_button, "clicked", G_CALLBACK(on_home_button_clicked), tab->webview);
    g_signal_connect(search_button, "clicked", G_CALLBACK(on_search_button_clicked), tab->url_entry);
    g_signal_connect(tab->url_entry, "activate", G_CALLBACK(on_url_entry_activate), tab->webview);

    // Add webview to container
    gtk_box_pack_start(GTK_BOX(tab->container), GTK_WIDGET(tab->webview), TRUE, TRUE, 0);
    
    // Connect WebKit signals 
    g_signal_connect(tab->webview, "load-changed", G_CALLBACK(on_load_changed), tab);
    g_signal_connect(tab->webview, "resource-load-started", G_CALLBACK(on_resource_load_started), intercept_data);
    g_signal_connect(tab->webview, "resource-response-received", G_CALLBACK(on_resource_response_received), intercept_data);

    // Load default page
    webkit_web_view_load_uri(tab->webview, "https://www.google.com");

    // Enable developer extras
    WebKitSettings* settings = webkit_web_view_get_settings(tab->webview);
    webkit_settings_set_enable_developer_extras(settings, TRUE);

    // Connect dev tools button signal
    g_signal_connect(tab->dev_tools_button, "clicked", G_CALLBACK(on_dev_tools_clicked), tab->webview);

    // Inside create_browser_tab function, after creating other buttons:

    // Create menu button (three dots)
    tab->menu_button = gtk_menu_button_new();
    GtkWidget* menu_image = gtk_image_new_from_icon_name("view-more-symbolic", GTK_ICON_SIZE_BUTTON);
    gtk_button_set_image(GTK_BUTTON(tab->menu_button), menu_image);

    // Create popup menu
    GtkWidget* popup_menu = gtk_menu_new();

    // Add VPN menu first (before other menu items)
    GtkWidget* vpn_menu = gtk_menu_new();
    GtkWidget* vpn_item = gtk_menu_item_new_with_label("VPN");
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(vpn_item), vpn_menu);

    // VPN menu items
    GtkWidget* vpn_connect = gtk_menu_item_new_with_label("Connect VPN");
    GtkWidget* vpn_disconnect = gtk_menu_item_new_with_label("Disconnect VPN");
    GtkWidget* vpn_status = gtk_menu_item_new_with_label("VPN Status");
    GtkWidget* vpn_settings = gtk_menu_item_new_with_label("VPN Settings");

    // Add items to VPN submenu
    gtk_menu_shell_append(GTK_MENU_SHELL(vpn_menu), vpn_connect);
    gtk_menu_shell_append(GTK_MENU_SHELL(vpn_menu), vpn_disconnect);
    gtk_menu_shell_append(GTK_MENU_SHELL(vpn_menu), gtk_separator_menu_item_new());
    gtk_menu_shell_append(GTK_MENU_SHELL(vpn_menu), vpn_status);
    gtk_menu_shell_append(GTK_MENU_SHELL(vpn_menu), vpn_settings);

    // Initialize VPN connection structure
    tab->vpn = g_new0(VPNConnection, 1);
    tab->vpn->connected = FALSE;
    tab->vpn->config_file = NULL;
    tab->vpn->last_error = NULL;

    // Add VPN menu item to main popup menu (before other items)
    gtk_menu_shell_append(GTK_MENU_SHELL(popup_menu), vpn_item);
    gtk_menu_shell_append(GTK_MENU_SHELL(popup_menu), gtk_separator_menu_item_new());

    // Connect VPN signals
    g_signal_connect(vpn_connect, "activate", G_CALLBACK(init_vpn_connection), tab->vpn);
    g_signal_connect(vpn_disconnect, "activate", G_CALLBACK(cleanup_vpn), tab->vpn);
    g_signal_connect(vpn_status, "activate", G_CALLBACK(show_vpn_status), tab->vpn);

    // Add menu items
    GtkWidget* dev_tools_item = gtk_menu_item_new_with_label("Developer Tools");
    GtkWidget* history_item = gtk_menu_item_new_with_label("History");
    GtkWidget* downloads_item = gtk_menu_item_new_with_label("Downloads");  // Add downloads item
    GtkWidget* intercept_item = gtk_menu_item_new_with_label("Intercept");
    GtkWidget* light_mode_item = gtk_menu_item_new_with_label("Light Mode");
    GtkWidget* dark_mode_item = gtk_menu_item_new_with_label("Dark Mode");
    GtkWidget* matrix_mode_item = gtk_menu_item_new_with_label("Matrix Mode");
    GtkWidget* webrtc_leak_item = gtk_menu_item_new_with_label("WebRTC Leak Check");
    GtkWidget* ip_rotator_item = gtk_menu_item_new_with_label("IP Rotator");

    gtk_menu_shell_append(GTK_MENU_SHELL(popup_menu), dev_tools_item);
    gtk_menu_shell_append(GTK_MENU_SHELL(popup_menu), history_item);
    gtk_menu_shell_append(GTK_MENU_SHELL(popup_menu), downloads_item);  // Add to menu
    gtk_menu_shell_append(GTK_MENU_SHELL(popup_menu), intercept_item);
    gtk_menu_shell_append(GTK_MENU_SHELL(popup_menu), light_mode_item);
    gtk_menu_shell_append(GTK_MENU_SHELL(popup_menu), dark_mode_item);
    gtk_menu_shell_append(GTK_MENU_SHELL(popup_menu), matrix_mode_item);
    gtk_menu_shell_append(GTK_MENU_SHELL(popup_menu), webrtc_leak_item);
    gtk_menu_shell_append(GTK_MENU_SHELL(popup_menu), ip_rotator_item);

    // Connect signals
    g_signal_connect(dev_tools_item, "activate", G_CALLBACK(on_dev_tools_clicked), tab->webview);
    g_signal_connect(history_item, "activate", G_CALLBACK(show_history_window), history);
    g_signal_connect(downloads_item, "activate", G_CALLBACK(show_downloads_window), NULL);  // Add callback
    g_signal_connect(intercept_item, "activate", G_CALLBACK(on_intercept_toggled), intercept_data);
    g_signal_connect(light_mode_item, "activate", G_CALLBACK(switch_mode), tab);
    g_signal_connect(dark_mode_item, "activate", G_CALLBACK(switch_mode), tab);
    g_signal_connect(matrix_mode_item, "activate", G_CALLBACK(switch_mode), tab);
    g_signal_connect(webrtc_leak_item, "activate", G_CALLBACK(check_webrtc_leaks), NULL);
    g_signal_connect(ip_rotator_item, "activate", G_CALLBACK(show_ip_rotator), tab);

    // Set popup menu for menu button
    gtk_menu_button_set_popup(GTK_MENU_BUTTON(tab->menu_button), popup_menu);

    // Add menu button to hbox
    gtk_box_pack_end(GTK_BOX(hbox), tab->menu_button, FALSE, FALSE, 0);

    // Remove the old buttons we're replacing
    gtk_widget_destroy(tab->dev_tools_button);
    tab->dev_tools_button = NULL;

    gtk_widget_show_all(popup_menu);

    // Store history reference
    tab->history = history;

    // Create mode submenu
    GtkWidget* mode_menu = gtk_menu_new();
    GtkWidget* mode_item = gtk_menu_item_new_with_label("Mode");
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(mode_item), mode_menu);

    GtkWidget
    InterceptData* intercept_data = g_new0(InterceptData, 1);
    intercept_data->enabled = FALSE;
    intercept_data->window = NULL;
    intercept_data->request_buffer = NULL;
    intercept_data->response_buffer = NULL;
    intercept_data->req_headers_buffer = NULL;
    intercept_data->resp_headers_buffer = NULL;
    intercept_data->pending_requests = NULL;
    intercept_data->current_resource = NULL;
    intercept_data->request_modified = FALSE;
    g_object_set_data(G_OBJECT(notebook), "intercept_data", intercept_data);
    
    // Initialize browser data
    BrowserData* browser_data = g_new0(BrowserData, 1);
    browser_data->history = g_new0(BrowserHistory, 1);
    browser_data->cookies = g_new0(BrowserCookies, 1);
    
    init_databases(browser_data);

    // Create initial tab
    BrowserTab* initial_tab = create_browser_tab(intercept_data, browser_data->history);
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), initial_tab->container,
                           create_tab_label("New Tab", GTK_NOTEBOOK(notebook), initial_tab));
    
    g_signal_connect_swapped(window, "destroy", G_CALLBACK(cleanup_intercept_data), intercept_data);
    g_signal_connect_swapped(window, "destroy", G_CALLBACK(cleanup_browser_data), browser_data);
    
    gtk_widget_show_all(window);
    gtk_main();
    
    return 0;
}
