#include <curl/curl.h>

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define DEFAULT_TANAKEN_URL "https://www.tanaken-1982.co.jp/ja/ir.html"
#define DEFAULT_INUNEKO_URL "https://corp.inuneko-seikatsu.co.jp/ir/"
#define DEFAULT_INUNEKO_API_URL \
    "https://www.xj-storage.jp/public-list/GetList2.aspx?company=AS09525&len=10000" \
    "&doctype=15%2C13%2C1%2C2%2C3%2C4%2C16%2C17%2C18%2C19%2C20%2C21%2C98%2C1120%2C1130" \
    "%2C1140%2C1150%2C1160%2C1170%2C1235%2C1236%2C15013%2C20001%2C0%2C5%2C6%2C8" \
    "%2C9%2C24%2C25%2C28%2C20000%2C34%2C90%2C91%2C92%2C93%2C95%2C96%2C97" \
    "%2C101%2C102%2C103%2C104%2C110%2C111%2C112%2C113%2C114%2C115%2C116%2C117" \
    "%2C118%2C119%2C120%2C723%2C1030%2C1040%2C1080%2C1090%2C1100%2C1135" \
    "%2C1136%2C1180%2C1190%2C1200%2C1210%2C1220%2C1230%2C1240%2C1250%2C1260" \
    "%2C1270%2C1280%2C1290%2C1300%2C1310%2C1320%2C1350%2C1360%2C1900%2C10091" \
    "%2C20091%2C20096%2C20098%2C20099%2C20111%2C20191%2C21900%2C34001%2C34002" \
    "%2C34100%2C34101%2C34102%2C14%2C200%2C20200&output=json&callback=tanakenAlert"
#define DEFAULT_STATE_FILE "state/seen.txt"
#define USER_AGENT "japanese-ir-alert/1.0"
#define MAX_RESPONSE_BYTES (16U * 1024U * 1024U)

typedef struct {
    char *data;
    size_t length;
    size_t capacity;
} TextBuffer;

typedef struct {
    char *id;
    char *company;
    char *source_url;
    char *date;
    char *title;
    char *url;
} Announcement;

typedef struct {
    Announcement *items;
    size_t count;
    size_t capacity;
} Announcements;

typedef struct {
    char **ids;
    size_t count;
    size_t capacity;
} SeenState;

typedef struct {
    const char *data;
    size_t length;
    size_t offset;
} UploadData;

typedef struct {
    const char *key;
    const char *company;
    const char *page_url;
} SourceConfig;

static void print_error(const char *message)
{
    fprintf(stderr, "Error: %s\n", message);
}

static void *checked_malloc(size_t size)
{
    void *result = malloc(size == 0 ? 1 : size);

    if (result == NULL) {
        print_error("out of memory");
        exit(EXIT_FAILURE);
    }

    return result;
}

static void *checked_realloc(void *pointer, size_t size)
{
    void *result = realloc(pointer, size == 0 ? 1 : size);

    if (result == NULL) {
        print_error("out of memory");
        exit(EXIT_FAILURE);
    }

    return result;
}

static char *duplicate_string(const char *value)
{
    size_t length;
    char *copy;

    if (value == NULL) {
        return NULL;
    }

    length = strlen(value);
    copy = checked_malloc(length + 1);
    memcpy(copy, value, length + 1);
    return copy;
}

static char *duplicate_range(const char *start, const char *end)
{
    size_t length;
    char *copy;

    if (start == NULL || end == NULL || end < start) {
        return NULL;
    }

    length = (size_t)(end - start);
    copy = checked_malloc(length + 1);
    memcpy(copy, start, length);
    copy[length] = '\0';
    return copy;
}

static int buffer_reserve(TextBuffer *buffer, size_t additional)
{
    size_t required;
    size_t capacity;

    if (additional > SIZE_MAX - buffer->length - 1) {
        return 0;
    }

    required = buffer->length + additional + 1;
    if (required <= buffer->capacity) {
        return 1;
    }

    capacity = buffer->capacity == 0 ? 4096 : buffer->capacity;
    while (capacity < required) {
        if (capacity > SIZE_MAX / 2) {
            capacity = required;
            break;
        }
        capacity *= 2;
    }

    buffer->data = checked_realloc(buffer->data, capacity);
    buffer->capacity = capacity;
    return 1;
}

static int buffer_append(TextBuffer *buffer, const char *data, size_t length)
{
    if (!buffer_reserve(buffer, length)) {
        return 0;
    }

    if (length > 0) {
        memcpy(buffer->data + buffer->length, data, length);
        buffer->length += length;
    }
    buffer->data[buffer->length] = '\0';
    return 1;
}

static int buffer_append_string(TextBuffer *buffer, const char *value)
{
    return buffer_append(buffer, value, strlen(value));
}

static int buffer_append_format(TextBuffer *buffer, const char *format, ...)
{
    va_list first;
    va_list second;
    int required;

    va_start(first, format);
    va_copy(second, first);
    required = vsnprintf(NULL, 0, format, first);
    va_end(first);

    if (required < 0 || !buffer_reserve(buffer, (size_t)required)) {
        va_end(second);
        return 0;
    }

    (void)vsnprintf(buffer->data + buffer->length,
                    buffer->capacity - buffer->length,
                    format,
                    second);
    va_end(second);
    buffer->length += (size_t)required;
    return 1;
}

static void free_buffer(TextBuffer *buffer)
{
    free(buffer->data);
    buffer->data = NULL;
    buffer->length = 0;
    buffer->capacity = 0;
}

static size_t page_write_callback(void *contents,
                                  size_t size,
                                  size_t item_count,
                                  void *user_data)
{
    TextBuffer *buffer = user_data;
    size_t total;

    if (size != 0 && item_count > SIZE_MAX / size) {
        return 0;
    }
    total = size * item_count;

    if (total > MAX_RESPONSE_BYTES || buffer->length > MAX_RESPONSE_BYTES - total) {
        return 0;
    }
    if (!buffer_append(buffer, contents, total)) {
        return 0;
    }
    return total;
}

static int fetch_page(const char *url, TextBuffer *response)
{
    CURL *curl;
    CURLcode result;
    char error_buffer[CURL_ERROR_SIZE] = {0};

    curl = curl_easy_init();
    if (curl == NULL) {
        print_error("could not initialize libcurl");
        return 0;
    }

    (void)curl_easy_setopt(curl, CURLOPT_URL, url);
    (void)curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    (void)curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
    (void)curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
    (void)curl_easy_setopt(curl, CURLOPT_USERAGENT, USER_AGENT);
    (void)curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 30L);
    (void)curl_easy_setopt(curl, CURLOPT_TIMEOUT, 60L);
    (void)curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
    (void)curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    (void)curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    (void)curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    (void)curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, error_buffer);
    (void)curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, page_write_callback);
    (void)curl_easy_setopt(curl, CURLOPT_WRITEDATA, response);

    result = curl_easy_perform(curl);
    if (result != CURLE_OK) {
        if (error_buffer[0] != '\0') {
            fprintf(stderr, "Error: could not fetch announcements: %s\n", error_buffer);
        } else {
            fprintf(stderr, "Error: could not fetch announcements: %s\n",
                    curl_easy_strerror(result));
        }
        curl_easy_cleanup(curl);
        return 0;
    }

    curl_easy_cleanup(curl);
    return response->length > 0;
}

static const char *find_in_range(const char *start,
                                 const char *end,
                                 const char *needle)
{
    size_t needle_length;
    const char *cursor;

    if (start == NULL || end == NULL || end < start || needle == NULL) {
        return NULL;
    }

    needle_length = strlen(needle);
    if (needle_length == 0) {
        return start;
    }

    for (cursor = start;
         cursor <= end && (size_t)(end - cursor) >= needle_length;
         cursor++) {
        if (memcmp(cursor, needle, needle_length) == 0) {
            return cursor;
        }
    }
    return NULL;
}

static int decode_entity(const char *start,
                         const char *end,
                         size_t *consumed,
                         unsigned char *replacement)
{
    static const struct {
        const char *encoded;
        unsigned char decoded;
    } entities[] = {
        {"&amp;", '&'},
        {"&quot;", '"'},
        {"&apos;", '\''},
        {"&#39;", '\''},
        {"&lt;", '<'},
        {"&gt;", '>'},
        {"&nbsp;", ' '}
    };
    size_t index;

    if (start >= end || *start != '&') {
        return 0;
    }

    for (index = 0; index < sizeof(entities) / sizeof(entities[0]); index++) {
        size_t length = strlen(entities[index].encoded);

        if ((size_t)(end - start) >= length &&
            memcmp(start, entities[index].encoded, length) == 0) {
            *consumed = length;
            *replacement = entities[index].decoded;
            return 1;
        }
    }
    return 0;
}

static char *normalized_text(const char *start, const char *end)
{
    char *result;
    size_t source_length;
    size_t output_length = 0;
    int pending_space = 0;
    int in_tag = 0;
    const char *cursor;

    if (start == NULL || end == NULL || end < start) {
        return NULL;
    }

    source_length = (size_t)(end - start);
    result = checked_malloc(source_length + 1);

    for (cursor = start; cursor < end; cursor++) {
        unsigned char character = (unsigned char)*cursor;
        size_t consumed = 0;
        unsigned char decoded = character;

        if (in_tag) {
            if (character == '>') {
                in_tag = 0;
            }
            continue;
        }
        if (character == '<') {
            in_tag = 1;
            continue;
        }
        if (decode_entity(cursor, end, &consumed, &decoded)) {
            cursor += consumed - 1;
            character = decoded;
        }

        if (isspace(character)) {
            if (output_length > 0) {
                pending_space = 1;
            }
            continue;
        }
        if (pending_space) {
            result[output_length++] = ' ';
            pending_space = 0;
        }
        result[output_length++] = (char)character;
    }

    while (output_length > 0 && result[output_length - 1] == ' ') {
        output_length--;
    }
    result[output_length] = '\0';
    return result;
}

static char *extract_attribute(const char *start,
                               const char *end,
                               const char *name)
{
    size_t name_length = strlen(name);
    const char *cursor = start;

    while (cursor != NULL && cursor < end) {
        const char *match = find_in_range(cursor, end, name);
        const char *value_start;
        const char *value_end;
        char quote;

        if (match == NULL) {
            return NULL;
        }
        if ((match > start &&
             (isalnum((unsigned char)match[-1]) || match[-1] == '-' ||
              match[-1] == '_')) ||
            match + name_length >= end) {
            cursor = match + 1;
            continue;
        }

        value_start = match + name_length;
        while (value_start < end && isspace((unsigned char)*value_start)) {
            value_start++;
        }
        if (value_start >= end || *value_start != '=') {
            cursor = match + 1;
            continue;
        }
        value_start++;
        while (value_start < end && isspace((unsigned char)*value_start)) {
            value_start++;
        }
        if (value_start >= end || (*value_start != '\'' && *value_start != '"')) {
            cursor = match + 1;
            continue;
        }

        quote = *value_start++;
        value_end = value_start;
        while (value_end < end && *value_end != quote) {
            value_end++;
        }
        if (value_end >= end) {
            return NULL;
        }
        return duplicate_range(value_start, value_end);
    }

    return NULL;
}

static char *extract_element_text(const char *start,
                                  const char *end,
                                  const char *element_marker)
{
    const char *marker;
    const char *text_start;
    const char *text_end;

    marker = find_in_range(start, end, element_marker);
    if (marker == NULL) {
        return NULL;
    }
    text_start = find_in_range(marker, end, ">");
    if (text_start == NULL) {
        return NULL;
    }
    text_start++;
    text_end = find_in_range(text_start, end, "</span>");
    if (text_end == NULL) {
        return NULL;
    }
    return normalized_text(text_start, text_end);
}

static int starts_with(const char *value, const char *prefix)
{
    size_t prefix_length = strlen(prefix);
    return strncmp(value, prefix, prefix_length) == 0;
}

static char *resolve_url(const char *base_url, const char *href)
{
    const char *scheme_end;
    const char *host_end;
    const char *last_slash;
    size_t prefix_length;
    char *result;

    if (starts_with(href, "https://") || starts_with(href, "http://")) {
        return duplicate_string(href);
    }

    scheme_end = strstr(base_url, "://");
    if (scheme_end == NULL) {
        return duplicate_string(href);
    }

    if (starts_with(href, "//")) {
        prefix_length = (size_t)(scheme_end - base_url);
        result = checked_malloc(prefix_length + strlen(href) + 1);
        memcpy(result, base_url, prefix_length);
        strcpy(result + prefix_length, href);
        return result;
    }

    if (href[0] == '/') {
        host_end = strchr(scheme_end + 3, '/');
        prefix_length = host_end == NULL ? strlen(base_url) : (size_t)(host_end - base_url);
        result = checked_malloc(prefix_length + strlen(href) + 1);
        memcpy(result, base_url, prefix_length);
        strcpy(result + prefix_length, href);
        return result;
    }

    last_slash = strrchr(base_url, '/');
    prefix_length = last_slash == NULL ? strlen(base_url) : (size_t)(last_slash - base_url + 1);
    result = checked_malloc(prefix_length + strlen(href) + 1);
    memcpy(result, base_url, prefix_length);
    strcpy(result + prefix_length, href);
    return result;
}

static void free_announcements(Announcements *announcements)
{
    size_t index;

    for (index = 0; index < announcements->count; index++) {
        free(announcements->items[index].id);
        free(announcements->items[index].company);
        free(announcements->items[index].source_url);
        free(announcements->items[index].date);
        free(announcements->items[index].title);
        free(announcements->items[index].url);
    }
    free(announcements->items);
    announcements->items = NULL;
    announcements->count = 0;
    announcements->capacity = 0;
}

static int announcement_exists(const Announcements *announcements, const char *id)
{
    size_t index;

    for (index = 0; index < announcements->count; index++) {
        if (strcmp(announcements->items[index].id, id) == 0) {
            return 1;
        }
    }
    return 0;
}

static char *make_announcement_id(const char *source_key, const char *url)
{
    size_t length = strlen(source_key) + strlen(url) + 2;
    char *id = checked_malloc(length);

    (void)snprintf(id, length, "%s|%s", source_key, url);
    return id;
}

static int add_announcement(Announcements *announcements,
                            char *id,
                            char *company,
                            char *source_url,
                            char *date,
                            char *title,
                            char *url)
{
    Announcement *item;

    if (announcement_exists(announcements, id)) {
        free(id);
        free(company);
        free(source_url);
        free(date);
        free(title);
        free(url);
        return 1;
    }

    if (announcements->count == announcements->capacity) {
        size_t new_capacity = announcements->capacity == 0 ? 16 : announcements->capacity * 2;
        announcements->items = checked_realloc(announcements->items,
                                               new_capacity * sizeof(*announcements->items));
        announcements->capacity = new_capacity;
    }

    item = &announcements->items[announcements->count++];
    item->id = id;
    item->company = company;
    item->source_url = source_url;
    item->date = date;
    item->title = title;
    item->url = url;
    return 1;
}

static int parse_tanaken_announcements(const char *html,
                                       const SourceConfig *source,
                                       Announcements *announcements)
{
    const char *list_marker;
    const char *list_start;
    const char *list_end;
    const char *cursor;

    list_marker = strstr(html, "class=\"tabItems\"");
    if (list_marker == NULL) {
        print_error("could not find the announcement list in the page");
        return 0;
    }
    list_start = strchr(list_marker, '>');
    if (list_start == NULL) {
        print_error("the announcement list is malformed");
        return 0;
    }
    list_start++;
    list_end = strstr(list_start, "</ul>");
    if (list_end == NULL) {
        print_error("the announcement list has no closing tag");
        return 0;
    }

    cursor = list_start;
    while (cursor < list_end) {
        const char *item_start = find_in_range(cursor, list_end, "<li");
        const char *item_end;
        char *date;
        char *href;
        char *title;
        char *url;
        char *id;

        if (item_start == NULL) {
            break;
        }
        item_end = find_in_range(item_start, list_end, "</li>");
        if (item_end == NULL) {
            print_error("an announcement item is malformed");
            return 0;
        }

        date = extract_attribute(item_start, item_end, "entrydate");
        if (date == NULL) {
            cursor = item_end + strlen("</li>");
            continue;
        }
        href = extract_attribute(item_start, item_end, "href");
        title = extract_element_text(item_start,
                                     item_end,
                                     "<span class=\"news_tx\">");
        if (href == NULL || title == NULL || title[0] == '\0') {
            free(date);
            free(href);
            free(title);
            cursor = item_end + strlen("</li>");
            continue;
        }

        url = resolve_url(source->page_url, href);
        free(href);
        id = make_announcement_id(source->key, url);
        if (!add_announcement(announcements,
                              id,
                              duplicate_string(source->company),
                              duplicate_string(source->page_url),
                              date,
                              title,
                              url)) {
            free(url);
            free(date);
            free(title);
            return 0;
        }

        cursor = item_end + strlen("</li>");
    }

    if (announcements->count == 0) {
        print_error("the announcement list contained no usable entries");
        return 0;
    }
    return 1;
}

static int json_hex_value(char character)
{
    if (character >= '0' && character <= '9') {
        return character - '0';
    }
    if (character >= 'a' && character <= 'f') {
        return character - 'a' + 10;
    }
    if (character >= 'A' && character <= 'F') {
        return character - 'A' + 10;
    }
    return -1;
}

static void append_utf8_codepoint(char *output, size_t *length, unsigned int codepoint)
{
    if (codepoint <= 0x7f) {
        output[(*length)++] = (char)codepoint;
    } else if (codepoint <= 0x7ff) {
        output[(*length)++] = (char)(0xc0 | (codepoint >> 6));
        output[(*length)++] = (char)(0x80 | (codepoint & 0x3f));
    } else if (codepoint <= 0xffff) {
        output[(*length)++] = (char)(0xe0 | (codepoint >> 12));
        output[(*length)++] = (char)(0x80 | ((codepoint >> 6) & 0x3f));
        output[(*length)++] = (char)(0x80 | (codepoint & 0x3f));
    } else {
        output[(*length)++] = (char)(0xf0 | (codepoint >> 18));
        output[(*length)++] = (char)(0x80 | ((codepoint >> 12) & 0x3f));
        output[(*length)++] = (char)(0x80 | ((codepoint >> 6) & 0x3f));
        output[(*length)++] = (char)(0x80 | (codepoint & 0x3f));
    }
}

static char *parse_json_string(const char **cursor_pointer, const char *end)
{
    const char *cursor = *cursor_pointer;
    size_t source_length;
    size_t output_length = 0;
    char *output;

    if (cursor >= end || *cursor != '"') {
        return NULL;
    }
    source_length = (size_t)(end - cursor);
    output = checked_malloc(source_length + 1);
    cursor++;

    while (cursor < end) {
        unsigned char character = (unsigned char)*cursor++;

        if (character == '"') {
            output[output_length] = '\0';
            *cursor_pointer = cursor;
            return output;
        }
        if (character != '\\') {
            output[output_length++] = (char)character;
            continue;
        }
        if (cursor >= end) {
            break;
        }

        character = (unsigned char)*cursor++;
        switch (character) {
        case '"':
        case '\\':
        case '/':
            output[output_length++] = (char)character;
            break;
        case 'b':
            output[output_length++] = '\b';
            break;
        case 'f':
            output[output_length++] = '\f';
            break;
        case 'n':
            output[output_length++] = '\n';
            break;
        case 'r':
            output[output_length++] = '\r';
            break;
        case 't':
            output[output_length++] = '\t';
            break;
        case 'u': {
            unsigned int codepoint = 0;
            int digit;
            int index;

            if ((size_t)(end - cursor) < 4) {
                free(output);
                return NULL;
            }
            for (index = 0; index < 4; index++) {
                digit = json_hex_value(cursor[index]);
                if (digit < 0) {
                    free(output);
                    return NULL;
                }
                codepoint = (codepoint << 4) | (unsigned int)digit;
            }
            cursor += 4;
            if (codepoint >= 0xd800 && codepoint <= 0xdbff &&
                (size_t)(end - cursor) >= 6 && cursor[0] == '\\' && cursor[1] == 'u') {
                unsigned int low = 0;

                for (index = 0; index < 4; index++) {
                    digit = json_hex_value(cursor[index + 2]);
                    if (digit < 0) {
                        low = 0;
                        break;
                    }
                    low = (low << 4) | (unsigned int)digit;
                }
                if (low >= 0xdc00 && low <= 0xdfff) {
                    codepoint = 0x10000 + ((codepoint - 0xd800) << 10) + (low - 0xdc00);
                    cursor += 6;
                }
            }
            if (codepoint >= 0xd800 && codepoint <= 0xdfff) {
                codepoint = 0xfffd;
            }
            append_utf8_codepoint(output, &output_length, codepoint);
            break;
        }
        default:
            free(output);
            return NULL;
        }
    }

    free(output);
    return NULL;
}

static const char *json_field_value_start(const char *start,
                                          const char *end,
                                          const char *field)
{
    char marker[128];
    const char *cursor = start;
    const char *match;
    size_t marker_length;

    if (strlen(field) + 3 >= sizeof(marker)) {
        return NULL;
    }
    (void)snprintf(marker, sizeof(marker), "\"%s\"", field);
    marker_length = strlen(marker);

    while ((match = find_in_range(cursor, end, marker)) != NULL) {
        cursor = match + marker_length;
        while (cursor < end && isspace((unsigned char)*cursor)) {
            cursor++;
        }
        if (cursor >= end || *cursor != ':') {
            continue;
        }
        cursor++;
        while (cursor < end && isspace((unsigned char)*cursor)) {
            cursor++;
        }
        return cursor;
    }
    return NULL;
}

static char *extract_json_string_field(const char *start,
                                       const char *end,
                                       const char *field)
{
    const char *value_start = json_field_value_start(start, end, field);

    if (value_start == NULL || value_start >= end || *value_start != '"') {
        return NULL;
    }
    return parse_json_string(&value_start, end);
}

static const char *find_json_matching(const char *start,
                                      const char *end,
                                      char opening,
                                      char closing)
{
    const char *cursor;
    size_t depth = 0;
    int in_string = 0;
    int escaped = 0;

    if (start == NULL || start >= end || *start != opening) {
        return NULL;
    }

    for (cursor = start; cursor < end; cursor++) {
        char character = *cursor;

        if (in_string) {
            if (escaped) {
                escaped = 0;
            } else if (character == '\\') {
                escaped = 1;
            } else if (character == '"') {
                in_string = 0;
            }
            continue;
        }
        if (character == '"') {
            in_string = 1;
        } else if (character == opening) {
            depth++;
        } else if (character == closing) {
            if (depth == 0) {
                return NULL;
            }
            depth--;
            if (depth == 0) {
                return cursor;
            }
        }
    }
    return NULL;
}

static char *extract_inuneko_file_url(const char *item_start, const char *item_end)
{
    const char *files_start = json_field_value_start(item_start, item_end, "files");
    const char *files_end;
    const char *cursor;
    char *fallback = NULL;

    if (files_start == NULL || *files_start != '[') {
        return NULL;
    }
    files_end = find_json_matching(files_start, item_end, '[', ']');
    if (files_end == NULL) {
        return NULL;
    }

    cursor = files_start + 1;
    while (cursor < files_end) {
        const char *object_end;
        char *type;
        char *url;

        while (cursor < files_end && (*cursor == ',' || isspace((unsigned char)*cursor))) {
            cursor++;
        }
        if (cursor >= files_end) {
            break;
        }
        if (*cursor != '{') {
            free(fallback);
            return NULL;
        }
        object_end = find_json_matching(cursor, files_end, '{', '}');
        if (object_end == NULL) {
            free(fallback);
            return NULL;
        }
        type = extract_json_string_field(cursor, object_end, "type");
        url = extract_json_string_field(cursor, object_end, "url");
        if (type != NULL && url != NULL && strcmp(type, "PDF-GENERAL") == 0) {
            free(type);
            free(fallback);
            return url;
        }
        if (type != NULL && url != NULL && strcmp(type, "HTML-GENERAL") == 0 && fallback == NULL) {
            fallback = url;
            url = NULL;
        }
        free(type);
        free(url);
        cursor = object_end + 1;
    }
    return fallback;
}

static int parse_inuneko_announcements(const char *json,
                                       const SourceConfig *source,
                                       Announcements *announcements)
{
    const char *json_end = json + strlen(json);
    const char *items_start = json_field_value_start(json, json_end, "items");
    const char *items_end;
    const char *cursor;

    if (items_start == NULL || *items_start != '[') {
        print_error("could not find the Inuneko announcement data");
        return 0;
    }
    items_end = find_json_matching(items_start, json_end, '[', ']');
    if (items_end == NULL) {
        print_error("the Inuneko announcement data is malformed");
        return 0;
    }

    cursor = items_start + 1;
    while (cursor < items_end) {
        const char *object_end;
        char *title;
        char *date;
        char *url;
        char *id;

        while (cursor < items_end && (*cursor == ',' || isspace((unsigned char)*cursor))) {
            cursor++;
        }
        if (cursor >= items_end) {
            break;
        }
        if (*cursor != '{') {
            print_error("an Inuneko announcement item is malformed");
            return 0;
        }
        object_end = find_json_matching(cursor, items_end, '{', '}');
        if (object_end == NULL) {
            print_error("an Inuneko announcement item is malformed");
            return 0;
        }
        title = extract_json_string_field(cursor, object_end, "title");
        date = extract_json_string_field(cursor, object_end, "publishDate");
        url = extract_inuneko_file_url(cursor, object_end);
        if (title == NULL || date == NULL || url == NULL || title[0] == '\0' || url[0] == '\0') {
            free(title);
            free(date);
            free(url);
            cursor = object_end + 1;
            continue;
        }

        id = make_announcement_id(source->key, url);
        if (!add_announcement(announcements,
                              id,
                              duplicate_string(source->company),
                              duplicate_string(source->page_url),
                              date,
                              title,
                              url)) {
            free(url);
            free(date);
            free(title);
            return 0;
        }
        cursor = object_end + 1;
    }

    if (announcements->count == 0) {
        print_error("the Inuneko announcement data contained no usable entries");
        return 0;
    }
    return 1;
}

static void free_state(SeenState *state)
{
    size_t index;

    for (index = 0; index < state->count; index++) {
        free(state->ids[index]);
    }
    free(state->ids);
    state->ids = NULL;
    state->count = 0;
    state->capacity = 0;
}

static int state_contains(const SeenState *state, const char *id)
{
    size_t index;

    for (index = 0; index < state->count; index++) {
        if (strcmp(state->ids[index], id) == 0) {
            return 1;
        }
    }
    return 0;
}

static int state_add(SeenState *state, const char *id)
{
    if (state_contains(state, id)) {
        return 1;
    }

    if (state->count == state->capacity) {
        size_t new_capacity = state->capacity == 0 ? 32 : state->capacity * 2;
        state->ids = checked_realloc(state->ids, new_capacity * sizeof(*state->ids));
        state->capacity = new_capacity;
    }
    state->ids[state->count++] = duplicate_string(id);
    return 1;
}

static int load_state(const char *path, SeenState *state, int *exists)
{
    FILE *file;
    char *line = NULL;
    size_t line_capacity = 0;
    ssize_t line_length;

    file = fopen(path, "r");
    if (file == NULL) {
        if (errno == ENOENT) {
            *exists = 0;
            return 1;
        }
        fprintf(stderr, "Error: could not read state file %s: %s\n", path, strerror(errno));
        return 0;
    }
    *exists = 1;

    while ((line_length = getline(&line, &line_capacity, file)) >= 0) {
        char *start = line;
        char *end = line + line_length;

        while (end > start && (end[-1] == '\n' || end[-1] == '\r')) {
            end--;
        }
        while (start < end && isspace((unsigned char)*start)) {
            start++;
        }
        while (end > start && isspace((unsigned char)end[-1])) {
            end--;
        }
        if (start < end) {
            char *id = duplicate_range(start, end);
            (void)state_add(state, id);
            free(id);
        }
    }

    free(line);
    if (ferror(file)) {
        fprintf(stderr, "Error: could not read state file %s\n", path);
        (void)fclose(file);
        return 0;
    }
    (void)fclose(file);
    return 1;
}

static int make_parent_directory(const char *path)
{
    char *copy;
    char *cursor;
    char *last_slash;

    copy = duplicate_string(path);
    last_slash = strrchr(copy, '/');
    if (last_slash == NULL) {
        free(copy);
        return 1;
    }
    *last_slash = '\0';
    if (copy[0] == '\0') {
        free(copy);
        return 1;
    }

    for (cursor = copy + 1; *cursor != '\0'; cursor++) {
        if (*cursor != '/') {
            continue;
        }
        *cursor = '\0';
        if (mkdir(copy, 0755) != 0 && errno != EEXIST) {
            fprintf(stderr, "Error: could not create directory %s: %s\n",
                    copy,
                    strerror(errno));
            free(copy);
            return 0;
        }
        *cursor = '/';
    }
    if (mkdir(copy, 0755) != 0 && errno != EEXIST) {
        fprintf(stderr, "Error: could not create directory %s: %s\n",
                copy,
                strerror(errno));
        free(copy);
        return 0;
    }
    free(copy);
    return 1;
}

static int write_state(const char *path, const SeenState *state)
{
    char *temporary_path;
    size_t temporary_length;
    int descriptor;
    FILE *file;
    size_t index;

    if (!make_parent_directory(path)) {
        return 0;
    }

    temporary_length = strlen(path) + strlen(".tmp.XXXXXX") + 1;
    temporary_path = checked_malloc(temporary_length);
    (void)snprintf(temporary_path, temporary_length, "%s.tmp.XXXXXX", path);
    descriptor = mkstemp(temporary_path);
    if (descriptor < 0) {
        fprintf(stderr, "Error: could not create temporary state file: %s\n", strerror(errno));
        free(temporary_path);
        return 0;
    }

    file = fdopen(descriptor, "w");
    if (file == NULL) {
        fprintf(stderr, "Error: could not open temporary state file: %s\n", strerror(errno));
        (void)close(descriptor);
        (void)unlink(temporary_path);
        free(temporary_path);
        return 0;
    }

    for (index = 0; index < state->count; index++) {
        if (fprintf(file, "%s\n", state->ids[index]) < 0) {
            fprintf(stderr, "Error: could not write state file %s\n", path);
            (void)fclose(file);
            (void)unlink(temporary_path);
            free(temporary_path);
            return 0;
        }
    }
    if (fflush(file) != 0) {
        fprintf(stderr, "Error: could not finish writing state file %s\n", path);
        (void)fclose(file);
        (void)unlink(temporary_path);
        free(temporary_path);
        return 0;
    }
    if (fsync(fileno(file)) != 0) {
        fprintf(stderr, "Error: could not finish writing state file %s\n", path);
        (void)fclose(file);
        (void)unlink(temporary_path);
        free(temporary_path);
        return 0;
    }
    if (fclose(file) != 0) {
        fprintf(stderr, "Error: could not finish writing state file %s\n", path);
        (void)unlink(temporary_path);
        free(temporary_path);
        return 0;
    }
    if (rename(temporary_path, path) != 0) {
        fprintf(stderr, "Error: could not replace state file %s: %s\n",
                path,
                strerror(errno));
        (void)unlink(temporary_path);
        free(temporary_path);
        return 0;
    }

    free(temporary_path);
    return 1;
}

static size_t upload_read_callback(char *buffer,
                                   size_t size,
                                   size_t item_count,
                                   void *user_data)
{
    UploadData *upload = user_data;
    size_t available;
    size_t requested;
    size_t amount;

    if (size != 0 && item_count > SIZE_MAX / size) {
        return CURL_READFUNC_ABORT;
    }
    requested = size * item_count;
    available = upload->length - upload->offset;
    amount = available < requested ? available : requested;
    if (amount > 0) {
        memcpy(buffer, upload->data + upload->offset, amount);
        upload->offset += amount;
    }
    return amount;
}

static int has_header_injection(const char *value)
{
    return value != NULL && (strchr(value, '\r') != NULL || strchr(value, '\n') != NULL);
}

static int add_mail_recipients(struct curl_slist **recipients, const char *value)
{
    char *copy;
    char *cursor;

    copy = duplicate_string(value);
    cursor = copy;
    while (*cursor != '\0') {
        char *start;
        char *end;
        char saved;
        struct curl_slist *new_list;

        while (*cursor == ' ' || *cursor == '\t' || *cursor == ',' || *cursor == ';') {
            cursor++;
        }
        if (*cursor == '\0') {
            break;
        }
        start = cursor;
        while (*cursor != '\0' && *cursor != ',' && *cursor != ';') {
            cursor++;
        }
        end = cursor;
        while (end > start && (end[-1] == ' ' || end[-1] == '\t')) {
            end--;
        }
        saved = *end;
        *end = '\0';
        if (*start != '\0') {
            new_list = curl_slist_append(*recipients, start);
            if (new_list == NULL) {
                free(copy);
                return 0;
            }
            *recipients = new_list;
        }
        *end = saved;
    }
    free(copy);
    return *recipients != NULL;
}

static int send_email(const char *smtp_url,
                      const char *username,
                      const char *password,
                      const char *from,
                      const char *to,
                      const char *subject_override,
                      const Announcements *announcements,
                      const SeenState *state)
{
    const char *subject = subject_override;
    const char *configured_subject;
    TextBuffer message = {0};
    struct curl_slist *recipients = NULL;
    UploadData upload;
    CURL *curl;
    CURLcode result;
    char error_buffer[CURL_ERROR_SIZE] = {0};
    size_t index;
    size_t new_count = 0;

    if (subject == NULL) {
        subject = "[TANAKEN / Inuneko Seikatsu] New IR announcement";
        configured_subject = getenv("ALERT_SUBJECT");
        if (configured_subject != NULL && configured_subject[0] != '\0') {
            subject = configured_subject;
        }
    }
    if (has_header_injection(subject) || has_header_injection(from) || has_header_injection(to)) {
        print_error("email header values must not contain newlines");
        return 0;
    }
    if (!add_mail_recipients(&recipients, to)) {
        curl_slist_free_all(recipients);
        print_error("ALERT_TO did not contain a valid recipient");
        return 0;
    }

    for (index = 0; index < announcements->count; index++) {
        if (!state_contains(state, announcements->items[index].id)) {
            new_count++;
        }
    }
    if (new_count == 0) {
        curl_slist_free_all(recipients);
        return 1;
    }

    if (!buffer_append_format(&message, "From: %s\r\n", from) ||
        !buffer_append_format(&message, "To: %s\r\n", to) ||
        !buffer_append_format(&message, "Subject: %s\r\n", subject) ||
        !buffer_append_string(&message, "MIME-Version: 1.0\r\n") ||
        !buffer_append_string(&message, "Content-Type: text/plain; charset=UTF-8\r\n") ||
        !buffer_append_string(&message, "Content-Transfer-Encoding: 8bit\r\n\r\n") ||
        !buffer_append_string(&message,
                              "A new announcement was found on a monitored IR page:\r\n\r\n")) {
        curl_slist_free_all(recipients);
        free_buffer(&message);
        print_error("could not build email message");
        return 0;
    }

    for (index = 0; index < announcements->count; index++) {
        const Announcement *announcement = &announcements->items[index];

        if (state_contains(state, announcement->id)) {
            continue;
        }
        if (!buffer_append_format(&message,
                                  "- [%s] %s | %s\r\n  PDF or document: %s\r\n  IR page: %s\r\n\r\n",
                                  announcement->company,
                                  announcement->date,
                                  announcement->title,
                                  announcement->url,
                                  announcement->source_url)) {
            curl_slist_free_all(recipients);
            free_buffer(&message);
            print_error("could not build email message");
            return 0;
        }
    }

    upload.data = message.data;
    upload.length = message.length;
    upload.offset = 0;

    curl = curl_easy_init();
    if (curl == NULL) {
        curl_slist_free_all(recipients);
        free_buffer(&message);
        print_error("could not initialize libcurl for SMTP");
        return 0;
    }

    (void)curl_easy_setopt(curl, CURLOPT_URL, smtp_url);
    (void)curl_easy_setopt(curl, CURLOPT_USERNAME, username == NULL ? "" : username);
    (void)curl_easy_setopt(curl, CURLOPT_PASSWORD, password == NULL ? "" : password);
    (void)curl_easy_setopt(curl, CURLOPT_MAIL_FROM, from);
    (void)curl_easy_setopt(curl, CURLOPT_MAIL_RCPT, recipients);
    (void)curl_easy_setopt(curl, CURLOPT_UPLOAD, 1L);
    (void)curl_easy_setopt(curl, CURLOPT_READFUNCTION, upload_read_callback);
    (void)curl_easy_setopt(curl, CURLOPT_READDATA, &upload);
    (void)curl_easy_setopt(curl, CURLOPT_INFILESIZE_LARGE, (curl_off_t)message.length);
    (void)curl_easy_setopt(curl, CURLOPT_USE_SSL, (long)CURLUSESSL_ALL);
    (void)curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 30L);
    (void)curl_easy_setopt(curl, CURLOPT_TIMEOUT, 60L);
    (void)curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    (void)curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, error_buffer);

    result = curl_easy_perform(curl);
    if (result != CURLE_OK) {
        if (error_buffer[0] != '\0') {
            fprintf(stderr, "Error: could not send email: %s\n", error_buffer);
        } else {
            fprintf(stderr, "Error: could not send email: %s\n",
                    curl_easy_strerror(result));
        }
        curl_easy_cleanup(curl);
        curl_slist_free_all(recipients);
        free_buffer(&message);
        return 0;
    }

    curl_easy_cleanup(curl);
    curl_slist_free_all(recipients);
    free_buffer(&message);
    return 1;
}

static const char *required_environment(const char *name)
{
    const char *value = getenv(name);

    if (value == NULL || value[0] == '\0') {
        fprintf(stderr, "Error: %s is required when an alert must be sent\n", name);
        return NULL;
    }
    return value;
}

static const char *environment_or_default(const char *name, const char *fallback)
{
    const char *value = getenv(name);

    return value == NULL || value[0] == '\0' ? fallback : value;
}

static int compare_announcements(const void *left_pointer, const void *right_pointer)
{
    const Announcement *left = left_pointer;
    const Announcement *right = right_pointer;
    int date_comparison = strcmp(right->date, left->date);

    if (date_comparison != 0) {
        return date_comparison;
    }
    return strcmp(left->id, right->id);
}

static void sort_announcements(Announcements *announcements)
{
    qsort(announcements->items,
          announcements->count,
          sizeof(*announcements->items),
          compare_announcements);
}

static int load_email_configuration(const char **smtp_url,
                                    const char **smtp_username,
                                    const char **smtp_password,
                                    const char **alert_from,
                                    const char **alert_to)
{
    *smtp_url = required_environment("SMTP_URL");
    *alert_from = required_environment("ALERT_FROM");
    *alert_to = required_environment("ALERT_TO");
    if (*smtp_url == NULL || *alert_from == NULL || *alert_to == NULL) {
        return 0;
    }

    *smtp_username = getenv("SMTP_USERNAME");
    *smtp_password = getenv("SMTP_PASSWORD");
    if ((*smtp_username == NULL || (*smtp_username)[0] == '\0') !=
        (*smtp_password == NULL || (*smtp_password)[0] == '\0')) {
        print_error("SMTP_USERNAME and SMTP_PASSWORD must be supplied together");
        return 0;
    }
    return 1;
}

static void print_usage(const char *program)
{
    printf("Usage: %s [--dry-run] [--initialize] [--test-latest]\n", program);
    printf("\n");
    printf("  --dry-run     Fetch and display entries without sending email or changing state.\n");
    printf("  --initialize  Add all current entries to state without sending email.\n");
    printf("  --test-latest Send a one-off email for the latest entry without changing state.\n");
}

static void print_entries(const Announcements *announcements, const SeenState *state)
{
    size_t index;

    for (index = 0; index < announcements->count; index++) {
        const Announcement *announcement = &announcements->items[index];
        printf("[%s] [%s] %s | %s\n",
               state_contains(state, announcement->id) ? "seen" : "new",
               announcement->company,
               announcement->date,
               announcement->title);
        printf("      %s\n", announcement->url);
    }
}

int main(int argc, char **argv)
{
    const char *tanaken_url;
    const char *inuneko_url;
    const char *inuneko_api_url;
    const char *state_path;
    const char *smtp_url;
    const char *smtp_username;
    const char *smtp_password;
    const char *alert_from;
    const char *alert_to;
    TextBuffer tanaken_html = {0};
    TextBuffer inuneko_json = {0};
    Announcements announcements = {0};
    SeenState state = {0};
    SourceConfig tanaken_source;
    SourceConfig inuneko_source;
    int state_exists = 0;
    int dry_run = 0;
    int initialize = 0;
    int test_latest = 0;
    int index;
    size_t latest_index = 0;
    size_t new_count = 0;
    int exit_code = EXIT_FAILURE;

    tanaken_url = environment_or_default("TANAKEN_URL", DEFAULT_TANAKEN_URL);
    inuneko_url = environment_or_default("INUNEKO_URL", DEFAULT_INUNEKO_URL);
    inuneko_api_url = environment_or_default("INUNEKO_API_URL", DEFAULT_INUNEKO_API_URL);
    state_path = environment_or_default("STATE_FILE", DEFAULT_STATE_FILE);
    tanaken_source.key = "tanaken";
    tanaken_source.company = "TANAKEN";
    tanaken_source.page_url = tanaken_url;
    inuneko_source.key = "inuneko";
    inuneko_source.company = "Inuneko Seikatsu";
    inuneko_source.page_url = inuneko_url;

    for (index = 1; index < argc; index++) {
        if (strcmp(argv[index], "--dry-run") == 0) {
            dry_run = 1;
        } else if (strcmp(argv[index], "--initialize") == 0) {
            initialize = 1;
        } else if (strcmp(argv[index], "--test-latest") == 0) {
            test_latest = 1;
        } else if (strcmp(argv[index], "--help") == 0 || strcmp(argv[index], "-h") == 0) {
            print_usage(argv[0]);
            return EXIT_SUCCESS;
        } else {
            fprintf(stderr, "Error: unknown argument %s\n", argv[index]);
            print_usage(argv[0]);
            return EXIT_FAILURE;
        }
    }

    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
        print_error("could not initialize libcurl");
        return EXIT_FAILURE;
    }

    if (!fetch_page(tanaken_url, &tanaken_html) ||
        !parse_tanaken_announcements(tanaken_html.data, &tanaken_source, &announcements) ||
        !fetch_page(inuneko_api_url, &inuneko_json) ||
        !parse_inuneko_announcements(inuneko_json.data, &inuneko_source, &announcements)) {
        goto cleanup;
    }
    sort_announcements(&announcements);

    if (test_latest) {
        if (dry_run) {
            printf("Latest announcement: %s | %s\n",
                   announcements.items[latest_index].date,
                   announcements.items[latest_index].title);
            printf("  %s\n", announcements.items[latest_index].url);
            exit_code = EXIT_SUCCESS;
            goto cleanup;
        }
        for (index = 0; index < (int)announcements.count; index++) {
            if ((size_t)index != latest_index) {
                (void)state_add(&state, announcements.items[index].id);
            }
        }
        if (!load_email_configuration(&smtp_url,
                                      &smtp_username,
                                      &smtp_password,
                                      &alert_from,
                                      &alert_to)) {
            goto cleanup;
        }
        if (!send_email(smtp_url,
                        smtp_username,
                        smtp_password,
                        alert_from,
                        alert_to,
                        environment_or_default("TEST_SUBJECT",
                                               "[TANAKEN / Inuneko Seikatsu] Latest IR announcement"),
                        &announcements,
                        &state)) {
            goto cleanup;
        }
        printf("Sent a test email for the latest announcement: %s\n",
               announcements.items[latest_index].title);
        exit_code = EXIT_SUCCESS;
        goto cleanup;
    }

    if (!load_state(state_path, &state, &state_exists)) {
        goto cleanup;
    }

    printf("Found %zu announcement(s).\n", announcements.count);
    if (dry_run) {
        print_entries(&announcements, &state);
        if (!state_exists) {
            printf("State file does not exist; a normal first run would initialize it without email.\n");
        }
        exit_code = EXIT_SUCCESS;
        goto cleanup;
    }

    if (!state_exists || initialize) {
        for (index = 0; index < (int)announcements.count; index++) {
            (void)state_add(&state, announcements.items[index].id);
        }
        if (!write_state(state_path, &state)) {
            goto cleanup;
        }
        printf("State initialized with %zu announcement(s); no email was sent.\n",
               announcements.count);
        exit_code = EXIT_SUCCESS;
        goto cleanup;
    }

    for (index = 0; index < (int)announcements.count; index++) {
        if (!state_contains(&state, announcements.items[index].id)) {
            new_count++;
        }
    }
    if (new_count == 0) {
        printf("No new announcements.\n");
        exit_code = EXIT_SUCCESS;
        goto cleanup;
    }

    if (!load_email_configuration(&smtp_url,
                                  &smtp_username,
                                  &smtp_password,
                                  &alert_from,
                                  &alert_to)) {
        goto cleanup;
    }
    if (!send_email(smtp_url,
                    smtp_username,
                    smtp_password,
                    alert_from,
                    alert_to,
                    NULL,
                    &announcements,
                    &state)) {
        goto cleanup;
    }

    for (index = 0; index < (int)announcements.count; index++) {
        (void)state_add(&state, announcements.items[index].id);
    }
    if (!write_state(state_path, &state)) {
        goto cleanup;
    }
    printf("Sent an email for %zu new announcement(s).\n", new_count);
    exit_code = EXIT_SUCCESS;

cleanup:
    free_buffer(&tanaken_html);
    free_buffer(&inuneko_json);
    free_announcements(&announcements);
    free_state(&state);
    curl_global_cleanup();
    return exit_code;
}
