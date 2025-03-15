// Add mode menu and related code
typedef enum {
    MODE_LIGHT,
    MODE_DARK, 
    MODE_MATRIX
} BrowserMode;

// Add to BrowserTab struct
typedef struct {
    // Existing fields...
    BrowserMode mode;
    GtkWidget* mode_item;  // Menu item
} BrowserTab;

// Add mode switcher function
static void switch_mode(GtkMenuItem* menu_item, BrowserTab* tab) {
    const gchar* mode_text = gtk_menu_item_get_label(menu_item);
    
    // Create CSS provider if not exists
    GtkCssProvider* provider = gtk_css_provider_new();
    
    const gchar* css;
    if (g_strcmp0(mode_text, "Light Mode") == 0) {
        tab->mode = MODE_LIGHT;
        css = "* { color: black; background-color: white; }";
    }
    else if (g_strcmp0(mode_text, "Dark Mode") == 0) {
        tab->mode = MODE_DARK;
        css = "* { color: #ccc; background-color: #222; }";
    }
    else if (g_strcmp0(mode_text, "Matrix Mode") == 0) {
        tab->mode = MODE_MATRIX;
        css = "* { color: #0f0; background-color: #000; font-family: monospace; }";
    }
    
    gtk_css_provider_load_from_data(provider, css, -1, NULL);
    
    // Apply to webview and toolbar
    GtkStyleContext* context = gtk_widget_get_style_context(GTK_WIDGET(tab->webview));
    gtk_style_context_add_provider(context, GTK_STYLE_PROVIDER(provider), 
                                 GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    
    context = gtk_widget_get_style_context(tab->container);
    gtk_style_context_add_provider(context, GTK_STYLE_PROVIDER(provider),
                                 GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    
    // Clean up
    g_object_unref(provider);
}

// In create_browser_tab, add mode menu:
static BrowserTab* create_browser_tab(InterceptData* intercept_data, BrowserHistory* history) {
    // Existing initialization...
    
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
    
    // Add mode menu before intercept item
    gtk_menu_shell_append(GTK_MENU_SHELL(popup_menu), mode_item);
    gtk_menu_shell_append(GTK_MENU_SHELL(popup_menu), intercept_item);

    // Store mode menu reference
    tab->mode_item = mode_item;
    tab->mode = MODE_LIGHT; // Default mode
    
    // Rest of function...
}