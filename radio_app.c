/*
 * Linux Internet Radio Player & Stream Downloader
 * * Dependencies: GTK+3, GStreamer 1.0, libcurl, json-c
 * * Compile with:
 * gcc -o radio_app radio_app.c $(pkg-config --cflags --libs gtk+-3.0 gstreamer-1.0 libcurl json-c) -lpthread
 */

#include <gtk/gtk.h>
#include <gst/gst.h>
#include <curl/curl.h>
#include <json-c/json.h>
#include <string.h>
#include <stdlib.h>
#include <pthread.h>
#include <ctype.h>

// -----------------------------------------------------------------------------
// Data Structures
// -----------------------------------------------------------------------------

typedef struct {
    GtkWidget *window;
    GtkWidget *search_entry;
    GtkWidget *search_type_combo;
    GtkWidget *results_tree;
    GtkListStore *list_store;
    GtkWidget *status_label;
    
    // Playback
    GstElement *pipeline;
    char current_url[2048];
    char current_name[256];
    
    // Recording (Downloading stream)
    gboolean is_recording;
    pthread_t record_thread;
} AppState;

struct MemoryStruct {
    char *memory;
    size_t size;
};

// -----------------------------------------------------------------------------
// Network & Helper Functions
// -----------------------------------------------------------------------------

// Callback for curl to write fetched data into memory
static size_t WriteMemoryCallback(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t realsize = size * nmemb;
    struct MemoryStruct *mem = (struct MemoryStruct *)userp;

    char *ptr = realloc(mem->memory, mem->size + realsize + 1);
    if (!ptr) return 0; // Out of memory

    mem->memory = ptr;
    memcpy(&(mem->memory[mem->size]), contents, realsize);
    mem->size += realsize;
    mem->memory[mem->size] = 0;

    return realsize;
}

// Callback for curl to write stream data directly to a file
static size_t WriteFileCallback(void *ptr, size_t size, size_t nmemb, void *stream) {
    return fwrite(ptr, size, nmemb, (FILE *)stream);
}

// Callback to abort curl transfer when recording stops
static int RecordProgressCallback(void *clientp, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal, curl_off_t ulnow) {
    AppState *app = (AppState *)clientp;
    return app->is_recording ? 0 : 1; // Returning 1 aborts the transfer
}

// Sanitize filename by replacing bad characters with underscores
void sanitize_filename(char *filename) {
    for (int i = 0; filename[i]; i++) {
        if (!isalnum(filename[i]) && filename[i] != '-' && filename[i] != '_') {
            filename[i] = '_';
        }
    }
}

static gboolean is_english_station(struct json_object *station) {
    struct json_object *j_language;

    if (!json_object_object_get_ex(station, "language", &j_language)) {
        return FALSE;
    }

    const char *language = json_object_get_string(j_language);
    if (!language) {
        return FALSE;
    }

    char *lower_language = g_ascii_strdown(language, -1);
    gboolean is_english = (strstr(lower_language, "english") != NULL);
    g_free(lower_language);

    return is_english;
}

static gboolean station_passes_live_test(struct json_object *station) {
    struct json_object *j_lastcheckok;

    if (!json_object_object_get_ex(station, "lastcheckok", &j_lastcheckok)) {
        return FALSE;
    }

    return json_object_get_int(j_lastcheckok) == 1;
}

static gboolean category_matches_search(struct json_object *station, const char *search_term) {
    struct json_object *j_tags;

    if (!json_object_object_get_ex(station, "tags", &j_tags)) {
        return FALSE;
    }

    const char *tags = json_object_get_string(j_tags);
    if (!tags || !search_term || !*search_term) {
        return FALSE;
    }

    char *normalized_tags = g_strdup(tags);
    for (char *p = normalized_tags; *p != '\0'; p++) {
        if (*p == ';' || *p == '|') {
            *p = ',';
        }
    }

    char **tag_list = g_strsplit(normalized_tags, ",", -1);
    gboolean match = FALSE;

    for (int i = 0; tag_list[i] != NULL; i++) {
        char *trimmed = g_strstrip(tag_list[i]);
        if (trimmed[0] == '\0') {
            continue;
        }

        if (g_ascii_strcasecmp(trimmed, search_term) == 0) {
            match = TRUE;
            break;
        }
    }

    g_strfreev(tag_list);
    g_free(normalized_tags);
    return match;
}

// -----------------------------------------------------------------------------
// Recording Thread
// -----------------------------------------------------------------------------

void* record_stream_thread(void* arg) {
    AppState *app = (AppState *)arg;
    
    char filename[512];
    char sanitized_name[256];
    strncpy(sanitized_name, app->current_name, sizeof(sanitized_name) - 1);
    sanitized_name[255] = '\0';
    sanitize_filename(sanitized_name);
    
    // Create a filename based on the station name
    snprintf(filename, sizeof(filename), "%s_stream.mp3", sanitized_name);
    
    FILE *f = fopen(filename, "wb");
    if (!f) {
        g_idle_add((GSourceFunc)gtk_label_set_text, app->status_label);
        // Safely update label from thread is tricky without g_idle_add, 
        // for simplicity we will just print to stderr
        fprintf(stderr, "Failed to open file for recording: %s\n", filename);
        app->is_recording = FALSE;
        return NULL;
    }

    CURL *curl = curl_easy_init();
    if (curl) {
        curl_easy_setopt(curl, CURLOPT_URL, app->current_url);
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteFileCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, f);
        
        // Progress callback to allow stopping
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, RecordProgressCallback);
        curl_easy_setopt(curl, CURLOPT_XFERINFODATA, app);

        g_print("Started downloading stream to: %s\n", filename);
        
        CURLcode res = curl_easy_perform(curl);
        
        if (res != CURLE_OK && res != CURLE_ABORTED_BY_CALLBACK) {
            fprintf(stderr, "curl error during recording: %s\n", curl_easy_strerror(res));
        }
        
        curl_easy_cleanup(curl);
    }
    
    fclose(f);
    app->is_recording = FALSE;
    g_print("Recording stopped. File saved as %s\n", filename);
    return NULL;
}

// -----------------------------------------------------------------------------
// GUI Callbacks
// -----------------------------------------------------------------------------

static void on_search_clicked(GtkWidget *widget, gpointer data) {
    AppState *app = (AppState *)data;
    const char *search_term = gtk_entry_get_text(GTK_ENTRY(app->search_entry));
    int search_type = gtk_combo_box_get_active(GTK_COMBO_BOX(app->search_type_combo));
    
    char *search_term_copy = g_strdup(search_term);
    char *trimmed_search = g_strstrip(search_term_copy);
    if (strlen(trimmed_search) == 0) {
        g_free(search_term_copy);
        return;
    }

    gtk_label_set_text(GTK_LABEL(app->status_label), "Searching directory...");
    
    CURL *curl = curl_easy_init();
    if (!curl) return;

    struct MemoryStruct chunk;
    chunk.memory = malloc(1);
    chunk.size = 0;

    char *normalized_search = g_ascii_strdown(trimmed_search, -1);

    // URL Encode the search term
    char *encoded_term = curl_easy_escape(curl, trimmed_search, 0);
    
    // Determine API Endpoint (Radio Browser API)
    char url[1024];
    if (search_type == 0) {
        // By Name
        snprintf(url, sizeof(url), "https://all.api.radio-browser.info/json/stations/byname/%s?limit=50&hidebroken=true", encoded_term);
    } else {
        // By Category / Tag (exact match against station tags)
        snprintf(url, sizeof(url), "https://all.api.radio-browser.info/json/stations/search?tag=%s&tagExact=true&limit=50&hidebroken=true", encoded_term);
    }
    
    curl_free(encoded_term);

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteMemoryCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&chunk);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "LinuxCRadioApp/1.0");
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L); // 10 sec timeout

    CURLcode res = curl_easy_perform(curl);

    if (res == CURLE_SSL_CONNECT_ERROR || res == CURLE_RECV_ERROR || res == CURLE_COULDNT_CONNECT) {
        /*
         * Keep HTTPS, but retry once without proxy settings.
         * This restores the previously-working direct-connect behavior in
         * environments where CONNECT tunneling is misconfigured.
         */
        curl_easy_setopt(curl, CURLOPT_PROXY, "");
        curl_easy_setopt(curl, CURLOPT_NOPROXY, "*");
        res = curl_easy_perform(curl);
    }

    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        fprintf(stderr, "curl search error: %s\n", curl_easy_strerror(res));
        gtk_label_set_text(GTK_LABEL(app->status_label), "Search failed! Network error.");
        g_free(normalized_search);
        g_free(search_term_copy);
        free(chunk.memory);
        return;
    }

    // Parse JSON
    gtk_list_store_clear(app->list_store);
    
    struct json_object *parsed_json = json_tokener_parse(chunk.memory);
    if (parsed_json && json_object_get_type(parsed_json) == json_type_array) {
        int n_stations = json_object_array_length(parsed_json);
        int shown_stations = 0;
        
        for (int i = 0; i < n_stations; i++) {
            struct json_object *station = json_object_array_get_idx(parsed_json, i);
            struct json_object *j_name, *j_url;
            
            if (json_object_object_get_ex(station, "name", &j_name) &&
                json_object_object_get_ex(station, "url_resolved", &j_url)) {
                if (!is_english_station(station) || !station_passes_live_test(station)) {
                    continue;
                }

                if (search_type == 1 && !category_matches_search(station, normalized_search)) {
                    continue;
                }
                
                const char *s_name = json_object_get_string(j_name);
                const char *s_url = json_object_get_string(j_url);
                
                GtkTreeIter iter;
                gtk_list_store_append(app->list_store, &iter);
                gtk_list_store_set(app->list_store, &iter, 0, s_name, 1, s_url, -1);
                shown_stations++;
            }
        }
        
        char status_msg[128];
        snprintf(status_msg, sizeof(status_msg), "Found %d live English stations (from %d total).", shown_stations, n_stations);
        gtk_label_set_text(GTK_LABEL(app->status_label), status_msg);
        
    } else {
        gtk_label_set_text(GTK_LABEL(app->status_label), "Error parsing directory data.");
    }

    if (parsed_json) json_object_put(parsed_json);
    g_free(normalized_search);
    g_free(search_term_copy);
    free(chunk.memory);
}

static void on_play_clicked(GtkWidget *widget, gpointer data) {
    AppState *app = (AppState *)data;
    
    GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(app->results_tree));
    GtkTreeModel *model;
    GtkTreeIter iter;
    
    if (gtk_tree_selection_get_selected(selection, &model, &iter)) {
        gchar *name, *url;
        gtk_tree_model_get(model, &iter, 0, &name, 1, &url, -1);
        
        strncpy(app->current_name, name, sizeof(app->current_name) - 1);
        strncpy(app->current_url, url, sizeof(app->current_url) - 1);
        
        // Stop current playback
        gst_element_set_state(app->pipeline, GST_STATE_NULL);
        
        // Set new URI and play
        g_object_set(G_OBJECT(app->pipeline), "uri", app->current_url, NULL);
        gst_element_set_state(app->pipeline, GST_STATE_PLAYING);
        
        char status[512];
        snprintf(status, sizeof(status), "Playing: %s", app->current_name);
        gtk_label_set_text(GTK_LABEL(app->status_label), status);
        
        g_free(name);
        g_free(url);
    }
}

static void on_stop_clicked(GtkWidget *widget, gpointer data) {
    AppState *app = (AppState *)data;
    gst_element_set_state(app->pipeline, GST_STATE_NULL);
    gtk_label_set_text(GTK_LABEL(app->status_label), "Stopped playback.");
}

static void on_record_clicked(GtkWidget *widget, gpointer data) {
    AppState *app = (AppState *)data;
    
    if (strlen(app->current_url) == 0) {
        gtk_label_set_text(GTK_LABEL(app->status_label), "Select and play a station first to record.");
        return;
    }
    
    if (app->is_recording) {
        gtk_label_set_text(GTK_LABEL(app->status_label), "Already downloading stream.");
        return;
    }

    app->is_recording = TRUE;
    pthread_create(&app->record_thread, NULL, record_stream_thread, app);
    
    char status[512];
    snprintf(status, sizeof(status), "Downloading stream: %s", app->current_name);
    gtk_label_set_text(GTK_LABEL(app->status_label), status);
}

static void on_stop_record_clicked(GtkWidget *widget, gpointer data) {
    AppState *app = (AppState *)data;
    if (app->is_recording) {
        app->is_recording = FALSE; // Thread callback will see this and abort curl
        pthread_join(app->record_thread, NULL);
        gtk_label_set_text(GTK_LABEL(app->status_label), "Download stopped.");
    }
}

// Ensure cleanup on window close
static void on_window_destroy(GtkWidget *widget, gpointer data) {
    AppState *app = (AppState *)data;
    gst_element_set_state(app->pipeline, GST_STATE_NULL);
    gst_object_unref(app->pipeline);
    
    if (app->is_recording) {
        app->is_recording = FALSE;
        pthread_join(app->record_thread, NULL);
    }
    
    gtk_main_quit();
}

// -----------------------------------------------------------------------------
// Main Application
// -----------------------------------------------------------------------------

int main(int argc, char *argv[]) {
    AppState app;
    memset(&app, 0, sizeof(AppState));

    // Initialize Libraries
    gtk_init(&argc, &argv);
    gst_init(&argc, &argv);
    curl_global_init(CURL_GLOBAL_ALL);

    // Create GStreamer Playbin
    app.pipeline = gst_element_factory_make("playbin", "player");
    if (!app.pipeline) {
        g_printerr("GStreamer: Failed to create playbin element. Are plugins installed?\n");
        return -1;
    }

    // Build GUI
    app.window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(app.window), "Internet Radio & Stream Downloader");
    gtk_window_set_default_size(GTK_WINDOW(app.window), 700, 500);
    g_signal_connect(app.window, "destroy", G_CALLBACK(on_window_destroy), &app);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
    gtk_container_set_border_width(GTK_CONTAINER(vbox), 10);
    gtk_container_add(GTK_CONTAINER(app.window), vbox);

    // Search Top Bar
    GtkWidget *hbox_search = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
    gtk_box_pack_start(GTK_BOX(vbox), hbox_search, FALSE, FALSE, 0);

    app.search_entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(app.search_entry), "Search term...");
    gtk_box_pack_start(GTK_BOX(hbox_search), app.search_entry, TRUE, TRUE, 0);

    app.search_type_combo = gtk_combo_box_text_new();
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(app.search_type_combo), "By Name");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(app.search_type_combo), "By Category");
    gtk_combo_box_set_active(GTK_COMBO_BOX(app.search_type_combo), 0);
    gtk_box_pack_start(GTK_BOX(hbox_search), app.search_type_combo, FALSE, FALSE, 0);

    GtkWidget *search_btn = gtk_button_new_with_label("Search Directory");
    g_signal_connect(search_btn, "clicked", G_CALLBACK(on_search_clicked), &app);
    gtk_box_pack_start(GTK_BOX(hbox_search), search_btn, FALSE, FALSE, 0);

    // Results TreeView
    GtkWidget *scrolled_window = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled_window), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_box_pack_start(GTK_BOX(vbox), scrolled_window, TRUE, TRUE, 0);

    app.list_store = gtk_list_store_new(2, G_TYPE_STRING, G_TYPE_STRING);
    app.results_tree = gtk_tree_view_new_with_model(GTK_TREE_MODEL(app.list_store));
    g_object_unref(app.list_store); // TreeView holds reference now

    GtkCellRenderer *renderer = gtk_cell_renderer_text_new();
    GtkTreeViewColumn *col_name = gtk_tree_view_column_new_with_attributes("Station Name", renderer, "text", 0, NULL);
    GtkTreeViewColumn *col_url = gtk_tree_view_column_new_with_attributes("Stream URL", renderer, "text", 1, NULL);
    
    gtk_tree_view_column_set_resizable(col_name, TRUE);
    gtk_tree_view_column_set_min_width(col_name, 250);
    
    gtk_tree_view_append_column(GTK_TREE_VIEW(app.results_tree), col_name);
    gtk_tree_view_append_column(GTK_TREE_VIEW(app.results_tree), col_url);
    gtk_container_add(GTK_CONTAINER(scrolled_window), app.results_tree);

    // Controls Bottom Bar
    GtkWidget *hbox_controls = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
    gtk_box_pack_start(GTK_BOX(vbox), hbox_controls, FALSE, FALSE, 0);

    GtkWidget *play_btn = gtk_button_new_with_label("▶ Play");
    GtkWidget *stop_btn = gtk_button_new_with_label("⏹ Stop");
    GtkWidget *record_btn = gtk_button_new_with_label("⏺ Download Stream");
    GtkWidget *stop_record_btn = gtk_button_new_with_label("⏹ Stop Download");

    g_signal_connect(play_btn, "clicked", G_CALLBACK(on_play_clicked), &app);
    g_signal_connect(stop_btn, "clicked", G_CALLBACK(on_stop_clicked), &app);
    g_signal_connect(record_btn, "clicked", G_CALLBACK(on_record_clicked), &app);
    g_signal_connect(stop_record_btn, "clicked", G_CALLBACK(on_stop_record_clicked), &app);

    gtk_box_pack_start(GTK_BOX(hbox_controls), play_btn, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(hbox_controls), stop_btn, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(hbox_controls), record_btn, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(hbox_controls), stop_record_btn, TRUE, TRUE, 0);

    // Status Label
    app.status_label = gtk_label_new("Ready. Search for a station.");
    gtk_box_pack_start(GTK_BOX(vbox), app.status_label, FALSE, FALSE, 0);

    // Show GUI and Start Main Loop
    gtk_widget_show_all(app.window);
    gtk_main();

    // Cleanup
    curl_global_cleanup();
    return 0;
}
