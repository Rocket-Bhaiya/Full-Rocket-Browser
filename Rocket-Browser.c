#include <gtk/gtk.h>
#include <webkit2/webkit2.h>
#include <stdio.h>
#include <glib.h>
#include <netdb.h>
#include <arpa/inet.h>

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

// Complete BrowserTab structure
typedef struct {
    GtkWidget* container;
    GtkWidget* url_entry;
    GtkComboBoxText* search_engine_combo;
    WebKitWebView* webview;
    GtkWidget* title_label;
    GtkWidget* progress_bar;
} BrowserTab;

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
static BrowserTab* create_browser_tab(InterceptData* intercept_data) {
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
    
    gtk_box_pack_start(GTK_BOX(hbox), back_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(hbox), forward_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(hbox), home_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(hbox), search_button, FALSE, FALSE, 0);

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
    g_signal_connect(tab->webview, "load-changed", G_CALLBACK(on_load_changed), NULL);
    g_signal_connect(tab->webview, "resource-load-started", G_CALLBACK(on_resource_load_started), intercept_data);
    g_signal_connect(tab->webview, "resource-response-received", G_CALLBACK(on_resource_response_received), intercept_data);

    // Load default page
    webkit_web_view_load_uri(tab->webview, "https://www.google.com");

    return tab;
}

// Function to create new tab label with close button
static GtkWidget* create_tab_label(const gchar* text, GtkNotebook* notebook, BrowserTab* tab) {
    GtkWidget* hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
    tab->title_label = gtk_label_new(text);
    GtkWidget* close_button = gtk_button_new_from_icon_name("window-close", GTK_ICON_SIZE_MENU);
    gtk_button_set_relief(GTK_BUTTON(close_button), GTK_RELIEF_NONE);
    
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
    switch (load_event) {
        case WEBKIT_LOAD_STARTED:
            printf("Load started\n");
            break;
        case WEBKIT_LOAD_COMMITTED:
            printf("Load committed\n");
            break;
        case WEBKIT_LOAD_FINISHED:
            printf("Load finished\n");
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
    BrowserTab* tab = create_browser_tab(intercept_data);
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

int main(int argc, char* argv[]) {
    gtk_init(&argc, &argv);
    
    // Create main window
    GtkWidget* window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_default_size(GTK_WINDOW(window), 1024, 768);
    gtk_window_set_title(GTK_WINDOW(window), "Rocket Browser");
    g_signal_connect(window, "destroy", G_CALLBACK(gtk_main_quit), NULL);
    gtk_window_set_icon_from_file(GTK_WINDOW(window), "images/favicon.ico", NULL);
    
    // Create main vertical box
    GtkWidget* main_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
    gtk_container_add(GTK_CONTAINER(window), main_vbox);
    
    // Create notebook for tabs
    GtkWidget* notebook = gtk_notebook_new();
    gtk_notebook_set_scrollable(GTK_NOTEBOOK(notebook), TRUE);
    gtk_box_pack_start(GTK_BOX(main_vbox), notebook, TRUE, TRUE, 0);
    
    // Create new tab button
    // GtkWidget* new_tab_button = gtk_button_new_with_label("+");
    // gtk_button_set_relief(GTK_BUTTON(new_tab_button), GTK_RELIEF_NONE);
    // g_signal_connect(new_tab_button, "clicked", G_CALLBACK(on_new_tab_clicked), notebook);
    // Replace the new tab button creation code with this:

// Create new tab button with larger size
    GtkWidget* new_tab_button = gtk_button_new_with_label("+");
    gtk_button_set_relief(GTK_BUTTON(new_tab_button), GTK_RELIEF_NONE);

    // Create a larger label for the button
    GtkWidget* button_label = gtk_bin_get_child(GTK_BIN(new_tab_button));
    PangoAttrList* attr_list = pango_attr_list_new();
    pango_attr_list_insert(attr_list, pango_attr_scale_new(1.5)); // Make text 1.5 times larger
    gtk_label_set_attributes(GTK_LABEL(button_label), attr_list);
    pango_attr_list_unref(attr_list);

    // Set minimum size for the button
    gtk_widget_set_size_request(new_tab_button, 40, 40);

// Add padding around the button content
    GtkCssProvider* provider = gtk_css_provider_new();
    const gchar* css = 
        "button { padding: 5px 10px; margin: 2px; }"
        "button label { font-weight: bold; }";
    gtk_css_provider_load_from_data(provider, css, -1, NULL);
    gtk_style_context_add_provider(
        gtk_widget_get_style_context(new_tab_button),
        GTK_STYLE_PROVIDER(provider),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION
    );
    g_object_unref(provider);

    g_signal_connect(new_tab_button, "clicked", G_CALLBACK(on_new_tab_clicked), notebook);
    // Add new tab button to notebook
    gtk_notebook_set_action_widget(GTK_NOTEBOOK(notebook), new_tab_button, GTK_PACK_END);
    gtk_widget_show(new_tab_button);

    // Create intercept toggle button
    GtkWidget* intercept_toggle = gtk_toggle_button_new_with_label("Intercept");
    gtk_box_pack_start(GTK_BOX(main_vbox), intercept_toggle, FALSE, FALSE, 0);

    // Create intercept data
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
    g_signal_connect(intercept_toggle, "toggled", G_CALLBACK(on_intercept_toggled), intercept_data);
    g_object_set_data(G_OBJECT(notebook), "intercept_data", intercept_data);
    
    // Create initial tab
    BrowserTab* initial_tab = create_browser_tab(intercept_data);
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), initial_tab->container,
                           create_tab_label("New Tab", GTK_NOTEBOOK(notebook), initial_tab));
    
    g_signal_connect_swapped(window, "destroy", G_CALLBACK(cleanup_intercept_data), intercept_data);

    gtk_widget_show_all(window);
    gtk_main();
    
    return 0;
}
