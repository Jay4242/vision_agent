// To compile: gcc main.c -o vision_agent -lX11 -lcurl -lcjson -lssl -lcrypto
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>   // For access
#include <time.h>     // Not strictly needed anymore for temp filenames but kept
#include <sys/stat.h> // For mkdir
#include <errno.h>    // For EEXIST

// X11 includes for keybinding
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h> // For X InternAtom

// libcurl and cJSON includes
#include <curl/curl.h>
#include <cjson/cJSON.h>

// Base64 encoding helper
#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/buffer.h>

// API Endpoints - as seen in Python scripts
#define LLM_BASE_URL "http://localhost:9090"
#define WHISPER_SERVER_URL "http://localhost:9191/inference"
// Vision model is gemma3:4b-it-q8_0, Text model is gemma-2-2b-it-q8_0
#define VISION_MODEL "gemma3:4b-it-q8_0"
#define TEXT_MODEL "gemma-2-2b-it-q8_0"

// --- Configuration ---
#define AUDIO_DURATION 5 // seconds
#define TEMP_DIR "/dev/shm/vision_agent"

// Global X11 variables
Display *dpy;
Window root;

// Function declarations
void setup_keybind(Display *dpy, Window root);
void handle_key_event(XEvent *event);
int take_screenshot(char *filepath_buffer, size_t buffer_size);
int record_audio(char *filepath_buffer, size_t buffer_size);
char* call_vision_api(const char *image_path, const char *prompt);
char* call_whisper_api(const char *audio_path, int translate);
char* call_llm_api(const char *system_prompt, const char *pre_prompt, const char *document, const char *post_prompt);

// X11 error handler
static int x_error_handler(Display *display, XErrorEvent *e) {
    char error_text[1024];
    XGetErrorText(display, e->error_code, error_text, sizeof(error_text));
    fprintf(stderr, "X11 Error: %s (Request Code: %d)\n", error_text, e->request_code);
    if (e->request_code == 33) { // 33 is the request code for X_GrabKey
        fprintf(stderr, "FATAL: Could not grab key combination. Another program (like the window manager) may already be using it.\n");
        exit(1);
    }
    return 0; // Return 0 to indicate the error has been handled
}


int main() {
    XEvent ev;

    printf("DEBUG: Starting main function.\n");

    // 1. Open connection to X server
    dpy = XOpenDisplay(NULL);
    if (!dpy) {
        fprintf(stderr, "Error: Could not open display.\n");
        return 1;
    }
    printf("DEBUG: Successfully connected to X display.\n");

    // Set the error handler *before* any X11 calls that might fail
    XSetErrorHandler(x_error_handler);

    root = DefaultRootWindow(dpy);

    // Create the temporary directory
    printf("DEBUG: Creating temporary directory: %s\n", TEMP_DIR);
    if (mkdir(TEMP_DIR, 0755) == -1) {
        if (errno != EEXIST) {
            perror("Error creating temporary directory");
            return 1;
        }
    }

    printf("Application started. Press Ctrl+Shift+F12 to trigger.\n");

    // 2. Setup keybinding
    setup_keybind(dpy, root);

    // Force a round-trip to the X server to catch any errors from setup_keybind
    XSync(dpy, False);

    // 3. Event loop
    printf("DEBUG: Entering event loop.\n");
    while (1) {
        XNextEvent(dpy, &ev);
        if (ev.type == KeyPress) {
            handle_key_event(&ev);
        }
    }

    // 4. Cleanup (unreachable in current loop, but good practice)
    printf("DEBUG: Closing X display connection.\n");
    XCloseDisplay(dpy);
    return 0;
}

void setup_keybind(Display *dpy, Window root) {
    printf("DEBUG: Setting up keybind...\n");
    // Determine keycode for 'F12' key
    // Using XKeysymToKeycode for robustness across layouts
    KeyCode keycode_F12 = XKeysymToKeycode(dpy, XK_F12);
    if (keycode_F12 == 0) {
        fprintf(stderr, "Warning: Could not find keycode for 'F12'. Keybind may not work.\n");
    } else {
        printf("DEBUG: Keycode for 'F12' is %d.\n", keycode_F12);
    }

    // Grab the key combination for different lock states (NumLock, CapsLock)
    unsigned int modifiers = ControlMask | ShiftMask;
    XGrabKey(dpy, keycode_F12, modifiers, root, False, GrabModeAsync, GrabModeAsync);
    XGrabKey(dpy, keycode_F12, modifiers | LockMask, root, False, GrabModeAsync, GrabModeAsync);
    XGrabKey(dpy, keycode_F12, modifiers | Mod2Mask, root, False, GrabModeAsync, GrabModeAsync);
    XGrabKey(dpy, keycode_F12, modifiers | LockMask | Mod2Mask, root, False, GrabModeAsync, GrabModeAsync);

    printf("Keybind Ctrl+Shift+F12 grabbed for all lock states. Listening for events...\n");
}

void handle_key_event(XEvent *event) {
    KeySym ks = XLookupKeysym(&event->xkey, 0); // Get KeySym from KeyPress event
    unsigned int state = event->xkey.state;

    // Clean the state mask to ignore modifiers like NumLock (Mod2Mask) and CapsLock (LockMask)
    unsigned int clean_state = state & ~(Mod2Mask | LockMask);

    // Check for Ctrl+Shift+F12
    if (ks == XK_F12 && clean_state == (ControlMask | ShiftMask)) {
        printf("Ctrl+Shift+F12 pressed! Initiating process...\n");

        char image_path[256];
        char audio_path[256];
        char *vision_response = NULL;
        char *whisper_response = NULL;
        char *command_prompt = NULL; // Stores the prompt derived from whisper or default

        // 1. Take Screenshot
        printf("DEBUG: Step 1: Taking screenshot...\n");
        if (!take_screenshot(image_path, sizeof(image_path))) {
            fprintf(stderr, "Failed to take screenshot. Aborting.\n");
            goto cleanup;
        }
        printf("Screenshot saved to: %s\n", image_path);

        // 2. Record Audio
        printf("\nDEBUG: Step 2: Recording audio...\n");
        if (!record_audio(audio_path, sizeof(audio_path))) {
            fprintf(stderr, "Failed to record audio. Aborting.\n");
            goto cleanup;
        }
        printf("Audio recorded to: %s\n", audio_path);

        // 3. Transcribe Audio
        printf("\nDEBUG: Step 3: Transcribing audio...\n");
        whisper_response = call_whisper_api(audio_path, 0); // 0 for no translation
        if (whisper_response && strlen(whisper_response) > 0) {
            printf("Whisper API Response: %s\n", whisper_response);
            command_prompt = strdup(whisper_response); // Use transcription as command
        } else {
            fprintf(stderr, "DEBUG: Whisper API call returned empty or NULL, using default prompt.\n");
            command_prompt = strdup("What is in this image?"); // Default prompt
        }

        // 4. Get Vision Response
        printf("\nDEBUG: Step 4: Calling Vision API with command: \"%s\"...\n", command_prompt);
        vision_response = call_vision_api(image_path, command_prompt);
        if (vision_response) {
            printf("Vision API Response: %s\n", vision_response);
        } else {
            fprintf(stderr, "DEBUG: Vision API call returned NULL.\n");
        }

cleanup: // Label for cleanup using goto
        printf("DEBUG: Cleaning up allocated memory for API responses.\n");
        if (command_prompt) free(command_prompt); // Free the strdup'd prompt
        if (vision_response) free(vision_response);
        if (whisper_response) free(whisper_response);
        printf("DEBUG: Process complete. Waiting for next keypress.\n\n");
    }
}

int take_screenshot(char *filepath_buffer, size_t buffer_size) {
    // Uses 'scrot' command for simplicity. Ensure it's installed.
    // scrot -o /dev/shm/vision_agent/capture.png
    snprintf(filepath_buffer, buffer_size, "%s/capture.png", TEMP_DIR);

    char command[512];
    // -o overwrites the file if it exists
    snprintf(command, sizeof(command), "scrot -o %s", filepath_buffer);

    printf("Executing: %s\n", command);
    int ret = system(command);
    if (ret == 0) {
        // Verify if the file was actually created.
        if (access(filepath_buffer, F_OK) == 0) {
            printf("DEBUG: Screenshot file created successfully.\n");
            return 1; // Success
        } else {
            fprintf(stderr, "Error: Screenshot command succeeded but file '%s' not found.\n", filepath_buffer);
            return 0;
        }
    } else {
        fprintf(stderr, "Error: scrot command failed with exit code %d\n", ret);
        return 0; // Failure
    }
}

int record_audio(char *filepath_buffer, size_t buffer_size) {
    // Uses 'ffmpeg' to record audio. You might need to adjust the input device.
    // Example: ffmpeg -f alsa -i default -t 5 -ar 16000 -ac 1 -c:a pcm_s16le /dev/shm/vision_agent/capture.wav
    snprintf(filepath_buffer, buffer_size, "%s/capture.wav", TEMP_DIR);

    // -f alsa -i default: Specifies ALSA input from the default device.
    // -t AUDIO_DURATION: Records for AUDIO_DURATION seconds.
    // -ar 16000: Audio sample rate of 16kHz.
    // -ac 1: Mono audio.
    // -c:a pcm_s16le: PCM signed 16-bit little-endian audio codec.
    // -y: Overwrite output file without asking.
    // -loglevel quiet: Suppress verbose ffmpeg output.
    char command[512];
    // Change -loglevel quiet to -loglevel info for more detailed ffmpeg output
    // Change -loglevel quiet to -loglevel info for more detailed ffmpeg output
    // Using specific device name from 'pactl list sources' output for microphone input
    snprintf(command, sizeof(command), "ffmpeg -f pulse -i alsa_input.pci-0000_00_1f.3.analog-stereo -t %d -ar 16000 -ac 1 -c:a pcm_s16le -y -loglevel info %s", AUDIO_DURATION, filepath_buffer);

    printf("Executing: %s\n", command);
    int ret = system(command);
    if (ret == 0) {
        // Verify if the file was actually created.
        if (access(filepath_buffer, F_OK) == 0) {
            printf("DEBUG: Audio file created successfully.\n");
            return 1; // Success
        } else {
            fprintf(stderr, "Error: Audio recording command succeeded but file '%s' not found.\n", filepath_buffer);
            return 0;
        }
    } else {
        fprintf(stderr, "Error: ffmpeg audio recording failed with exit code %d\n", ret);
        return 0; // Failure
    }
}

// Helper function to base64 encode a file
char* base64_encode_file(const char *filepath) {
    printf("DEBUG: Base64 encoding file: %s\n", filepath);
    BIO *bio, *b64;
    FILE *fp = fopen(filepath, "rb");
    if (!fp) {
        perror("fopen");
        return NULL;
    }

    // Get file size
    fseek(fp, 0, SEEK_END);
    long file_size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    printf("DEBUG: File size for encoding: %ld bytes.\n", file_size);

    char *file_buffer = malloc(file_size);
    if (!file_buffer) {
        fclose(fp);
        fprintf(stderr, "Failed to allocate memory for file buffer.\n");
        return NULL;
    }

    if (fread(file_buffer, 1, file_size, fp) != file_size) {
        fclose(fp);
        free(file_buffer);
        fprintf(stderr, "Failed to read file.\n");
        return NULL;
    }
    fclose(fp);

    // Base64 encode
    b64 = BIO_new(BIO_f_base64());
    BIO_set_flags(b64, BIO_FLAGS_BASE64_NO_NL);
    bio = BIO_new(BIO_s_mem());
    BIO_push(b64, bio);
    BIO_write(b64, file_buffer, file_size);
    BIO_flush(b64);

    BUF_MEM *buffer_ptr;
    BIO_get_mem_ptr(b64, &buffer_ptr);

    char *encoded_data = malloc(buffer_ptr->length + 1);
    if (encoded_data) {
        memcpy(encoded_data, buffer_ptr->data, buffer_ptr->length);
        encoded_data[buffer_ptr->length] = '\0';
        printf("DEBUG: Base64 encoding successful.\n");
    } else {
        fprintf(stderr, "DEBUG: Failed to allocate memory for encoded data.\n");
    }

    BIO_free_all(b64);
    free(file_buffer);

    return encoded_data;
}

// Struct to hold memory buffer for curl response
struct MemoryStruct {
    char *memory;
    size_t size;
};

// Callback function for curl to write data into a memory buffer
static size_t WriteMemoryCallback(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t realsize = size * nmemb;
    struct MemoryStruct *mem = (struct MemoryStruct *)userp;

    char *ptr = realloc(mem->memory, mem->size + realsize + 1);
    if (ptr == NULL) {
        /* out of memory! */
        printf("not enough memory (realloc returned NULL)\n");
        return 0;
    }

    mem->memory = ptr;
    memcpy(&(mem->memory[mem->size]), contents, realsize);
    mem->size += realsize;
    mem->memory[mem->size] = 0;

    return realsize;
}


char* call_vision_api(const char *image_path, const char *prompt) {
    CURL *curl;
    CURLcode res;
    char *response_string = NULL;
    printf("DEBUG: Preparing Vision API call.\n");

    // 1. Base64 encode the image
    char *base64_image = base64_encode_file(image_path);
    if (!base64_image) {
        fprintf(stderr, "Failed to base64 encode image.\n");
        return NULL;
    }

    curl_global_init(CURL_GLOBAL_ALL);
    curl = curl_easy_init();
    if (curl) {
        struct MemoryStruct chunk;
        chunk.memory = malloc(1);
        chunk.size = 0;

        // 2. Construct JSON payload
        cJSON *root = cJSON_CreateObject();
        cJSON_AddStringToObject(root, "model", VISION_MODEL);
        cJSON_AddBoolToObject(root, "stream", cJSON_False);

        cJSON *messages_array = cJSON_CreateArray();
        cJSON *user_message = cJSON_CreateObject();
        cJSON_AddStringToObject(user_message, "role", "user");

        cJSON *content_array = cJSON_CreateArray();

        // Add text part
        cJSON *text_part = cJSON_CreateObject();
        cJSON_AddStringToObject(text_part, "type", "text");
        cJSON_AddStringToObject(text_part, "text", prompt);
        cJSON_AddItemToArray(content_array, text_part);

        // Add image part
        cJSON *image_part = cJSON_CreateObject();
        cJSON_AddStringToObject(image_part, "type", "image_url");
        cJSON *image_url_obj = cJSON_CreateObject();

        // Construct the full data URI: data:image/png;base64, followed by the base64_image string
        size_t data_uri_prefix_len = strlen("data:image/png;base64,");
        size_t base64_image_len = strlen(base64_image);
        char *full_data_uri = malloc(data_uri_prefix_len + base64_image_len + 1);
        if (!full_data_uri) {
            fprintf(stderr, "Failed to allocate memory for full_data_uri.\n");
            cJSON_Delete(root);
            free(base64_image);
            return NULL;
        }
        strcpy(full_data_uri, "data:image/png;base64,");
        strcat(full_data_uri, base64_image);
        cJSON_AddStringToObject(image_url_obj, "url", full_data_uri);
        free(full_data_uri); // cJSON copies the string, so we can free it.

        cJSON_AddItemToObject(image_part, "image_url", image_url_obj);
        cJSON_AddItemToArray(content_array, image_part);

        cJSON_AddItemToObject(user_message, "content", content_array);
        cJSON_AddItemToArray(messages_array, user_message);
        cJSON_AddItemToObject(root, "messages", messages_array);

        char *json_payload = cJSON_PrintUnformatted(root);
        // Updated debug print for new JSON structure
        printf("DEBUG: Vision API JSON Payload (image data omitted): {\"model\":\"%s\",\"messages\":[{\"role\":\"user\",\"content\":[{\"type\":\"text\",\"text\":\"%s\"},{\"type\":\"image_url\",\"image_url\":{\"url\":\"data:image/png;base64,...\"}}]}]}\n", VISION_MODEL, prompt);


        // 3. Set up curl request
        char url[256];
        snprintf(url, sizeof(url), "%s/v1/chat/completions", LLM_BASE_URL);
        printf("DEBUG: Vision API URL: %s\n", url);

        struct curl_slist *headers = NULL;
        headers = curl_slist_append(headers, "Content-Type: application/json");

        curl_easy_setopt(curl, CURLOPT_URL, url);
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json_payload);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteMemoryCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&chunk);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 10000L); // 10 second connection timeout
        curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 120000L);      // 120 second total timeout

        // 4. Perform the request
        printf("DEBUG: Executing Vision API request...\n");
        res = curl_easy_perform(curl);
        if (res != CURLE_OK) {
            fprintf(stderr, "curl_easy_perform() failed: %s\n", curl_easy_strerror(res));
        } else {
            printf("DEBUG: Vision API raw response: %s\n", chunk.memory);
            // 5. Parse JSON response
            // Handle streaming format response, line by line
            char *full_response_content = strdup(""); // Buffer to accumulate content
            if (!full_response_content) {
                fprintf(stderr, "Failed to allocate memory for full_response_content.\n");
                goto cleanup_curl;
            }

            char *line_start = chunk.memory;
            char *next_line;

            while (line_start && (next_line = strstr(line_start, "\n")) != NULL) {
                *next_line = '\0'; // Null-terminate the current line
                char *current_line_ptr = line_start;

                // Skip "data: " prefix
                if (strncmp(current_line_ptr, "data: ", 6) == 0) {
                    current_line_ptr += 6;
                } else if (strncmp(current_line_ptr, "data:", 5) == 0) { // Handle "data:" without space too
                    current_line_ptr += 5;
                }

                // Skip "[DONE]" line
                if (strcmp(current_line_ptr, "[DONE]") == 0) {
                    printf("DEBUG: End of stream received.\n");
                    break;
                }

                cJSON *json_chunk = cJSON_Parse(current_line_ptr);
                if (json_chunk) {
                    const cJSON *choices = cJSON_GetObjectItemCaseSensitive(json_chunk, "choices");
                    if (cJSON_IsArray(choices) && cJSON_GetArraySize(choices) > 0) {
                        const cJSON *choice = cJSON_GetArrayItem(choices, 0);
                        if (choice) {
                            const cJSON *delta = cJSON_GetObjectItemCaseSensitive(choice, "delta");
                            if (delta) {
                                const cJSON *content = cJSON_GetObjectItemCaseSensitive(delta, "content");
                                if (cJSON_IsString(content) && content->valuestring != NULL) {
                                    // Append content to full_response_content
                                    size_t current_len = strlen(full_response_content);
                                    size_t new_content_len = strlen(content->valuestring);
                                    char *temp = realloc(full_response_content, current_len + new_content_len + 1);
                                    if (temp) {
                                        full_response_content = temp;
                                        strcat(full_response_content, content->valuestring);
                                    } else {
                                        fprintf(stderr, "Failed to reallocate memory for full_response_content.\n");
                                        free(full_response_content);
                                        full_response_content = NULL; // Indicate failure
                                        cJSON_Delete(json_chunk);
                                        goto cleanup_curl;
                                    }
                                }
                            }
                        }
                    }
                    cJSON_Delete(json_chunk);
                } else {
                    // This can happen for empty lines or malformed chunks
                    // fprintf(stderr, "DEBUG: Failed to parse JSON chunk: %s\n", current_line_ptr);
                }
                line_start = next_line + 1; // Move to the beginning of the next line
            }
            response_string = full_response_content; // Assign the accumulated content
            printf("DEBUG: Successfully extracted full response from Vision API stream.\n");
        }

cleanup_curl: // Label for cleanup
        curl_slist_free_all(headers);
        cJSON_Delete(root);
        free(json_payload);
        free(chunk.memory);
        curl_easy_cleanup(curl);
    }

    free(base64_image);
    curl_global_cleanup();

    return response_string;
}

char* call_whisper_api(const char *audio_path, int translate) {
    CURL *curl;
    CURLcode res;
    char *response_string = NULL;
    printf("DEBUG: Preparing Whisper API call for file: %s\n", audio_path);

    curl_global_init(CURL_GLOBAL_ALL);
    curl = curl_easy_init();
    if (curl) {
        struct MemoryStruct chunk;
        chunk.memory = malloc(1);
        chunk.size = 0;

        // Create multipart form
        curl_mime *form = curl_mime_init(curl);
        curl_mimepart *field = NULL;

        // Add file part
        printf("DEBUG: Adding file '%s' to multipart form.\n", audio_path);
        field = curl_mime_addpart(form);
        curl_mime_name(field, "file");
        curl_mime_filedata(field, audio_path);

        // Add translate part
        printf("DEBUG: Adding 'translate'=%s to multipart form.\n", translate ? "true" : "false");
        field = curl_mime_addpart(form);
        curl_mime_name(field, "translate");
        curl_mime_data(field, translate ? "true" : "false", -1);


        // Set up curl request
        printf("DEBUG: Whisper API URL: %s\n", WHISPER_SERVER_URL);
        curl_easy_setopt(curl, CURLOPT_URL, WHISPER_SERVER_URL);
        curl_easy_setopt(curl, CURLOPT_MIMEPOST, form);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteMemoryCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&chunk);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 10000L); // 10 second connection timeout
        curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 120000L);      // 120 second total timeout

        // Perform the request
        printf("DEBUG: Executing Whisper API request...\n");
        res = curl_easy_perform(curl);
        if (res != CURLE_OK) {
            fprintf(stderr, "curl_easy_perform() failed: %s\n", curl_easy_strerror(res));
        } else {
            printf("DEBUG: Whisper API raw response: %s\n", chunk.memory);
            // The response from whisper is a JSON object with a "text" field
            cJSON *json_response = cJSON_Parse(chunk.memory);
            if (json_response) {
                const cJSON *text_field = cJSON_GetObjectItem(json_response, "text");
                if (cJSON_IsString(text_field) && (text_field->valuestring != NULL)) {
                    response_string = strdup(text_field->valuestring);
                    printf("DEBUG: Successfully parsed 'text' field from Whisper API JSON.\n");
                } else {
                    fprintf(stderr, "DEBUG: Could not find 'text' string in Whisper API JSON.\n");
                }
                cJSON_Delete(json_response);
            } else {
                fprintf(stderr, "DEBUG: Failed to parse Whisper API response as JSON.\n");
            }
        }

        // Cleanup
        curl_mime_free(form);
        free(chunk.memory);
        curl_easy_cleanup(curl);
    }

    curl_global_cleanup();

    return response_string;
}

char* call_llm_api(const char *system_prompt, const char *pre_prompt, const char *document, const char *post_prompt) {
    CURL *curl;
    CURLcode res;
    char *response_string = NULL;
    printf("DEBUG: Preparing LLM API call.\n");

    curl_global_init(CURL_GLOBAL_ALL);
    curl = curl_easy_init();
    if (curl) {
        struct MemoryStruct chunk;
        chunk.memory = malloc(1);
        chunk.size = 0;

        // Construct JSON payload
        cJSON *root = cJSON_CreateObject();
        cJSON_AddStringToObject(root, "model", TEXT_MODEL);
        cJSON_AddBoolToObject(root, "stream", cJSON_False);

        cJSON *messages = cJSON_CreateArray();

        // System Prompt
        if (system_prompt) {
            cJSON *system_message = cJSON_CreateObject();
            cJSON_AddStringToObject(system_message, "role", "system");
            cJSON_AddStringToObject(system_message, "content", system_prompt);
            cJSON_AddItemToArray(messages, system_message);
        }

        // User Prompt (constructed from parts)
        cJSON *user_message = cJSON_CreateObject();
        cJSON_AddStringToObject(user_message, "role", "user");

        // Combine prompts into a single string
        // A more robust solution would handle memory allocation better
        char combined_prompt[4096] = {0};
        if (pre_prompt) strncat(combined_prompt, pre_prompt, sizeof(combined_prompt) - strlen(combined_prompt) - 1);
        if (document) strncat(combined_prompt, document, sizeof(combined_prompt) - strlen(combined_prompt) - 1);
        if (post_prompt) strncat(combined_prompt, post_prompt, sizeof(combined_prompt) - strlen(combined_prompt) - 1);

        cJSON_AddStringToObject(user_message, "content", combined_prompt);
        cJSON_AddItemToArray(messages, user_message);

        cJSON_AddItemToObject(root, "messages", messages);


        char *json_payload = cJSON_PrintUnformatted(root);
        printf("DEBUG: LLM API JSON Payload: %s\n", json_payload);

        // Set up curl request
        char url[256];
        snprintf(url, sizeof(url), "%s/v1/chat/completions", LLM_BASE_URL);
        printf("DEBUG: LLM API URL: %s\n", url);

        struct curl_slist *headers = NULL;
        headers = curl_slist_append(headers, "Content-Type: application/json");

        curl_easy_setopt(curl, CURLOPT_URL, url);
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json_payload);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteMemoryCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&chunk);

        // Perform the request
        printf("DEBUG: Executing LLM API request...\n");
        res = curl_easy_perform(curl);
        if (res != CURLE_OK) {
            fprintf(stderr, "curl_easy_perform() failed: %s\n", curl_easy_strerror(res));
        } else {
            printf("DEBUG: LLM API raw response: %s\n", chunk.memory);
            // Parse JSON response
            cJSON *json_response = cJSON_Parse(chunk.memory);
            if (json_response) {
                const cJSON *message = cJSON_GetObjectItem(json_response, "message");
                if (message) {
                    const cJSON *content = cJSON_GetObjectItem(message, "content");
                    if (cJSON_IsString(content) && (content->valuestring != NULL)) {
                        response_string = strdup(content->valuestring);
                        printf("DEBUG: Successfully parsed 'content' from LLM API JSON.\n");
                    } else {
                        fprintf(stderr, "DEBUG: Could not find 'content' string in LLM API JSON message.\n");
                    }
                } else {
                    fprintf(stderr, "DEBUG: Could not find 'message' object in LLM API JSON.\n");
                }
                cJSON_Delete(json_response);
            } else {
                fprintf(stderr, "DEBUG: Failed to parse LLM API response as JSON.\n");
            }
        }

        // Cleanup
        curl_slist_free_all(headers);
        cJSON_Delete(root);
        free(json_payload);
        free(chunk.memory);
        curl_easy_cleanup(curl);
    }

    curl_global_cleanup();

    return response_string;
}
