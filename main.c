#include "mongoose.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_ITEMS 1000
#define DATA_FILE "inventory.dat"
#define CSV_FILE  "inventory_report.csv"
#define LISTEN_URL "http://127.0.0.1:8000"

typedef struct {
    int id;
    char name[64];
    int qty;
    int min_stock;
    float price;
} Item;

Item inventory[MAX_ITEMS];
int item_count = 0;
int next_id = 1001;

// Binary search on the id-sorted array
int binary_search_id(int target_id) {
    int lo = 0, hi = item_count - 1;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        if (inventory[mid].id == target_id) return mid;
        if (inventory[mid].id < target_id) lo = mid + 1;
        else hi = mid - 1;
    }
    return -1;
}

// Saves data to .dat file
int save_data(void) {
    FILE *f = fopen(DATA_FILE, "wb");
    if (!f) return 0;
    int ok = fwrite(&item_count, sizeof(int), 1, f) == 1 &&
             fwrite(&next_id, sizeof(int), 1, f) == 1 &&
             (item_count == 0 ||
              fwrite(inventory, sizeof(Item), (size_t) item_count, f) == (size_t) item_count);
    fclose(f);
    return ok;
}

// Reads inventory.dat at startup(if it exists otherwise empty table), starts empty if the file is missing or corrupt
void load_data(void) {
    FILE *f = fopen(DATA_FILE, "rb");
    if (!f) return;
    int count = 0, nid = 1001;
    if (fread(&count, sizeof(int), 1, f) == 1 &&
        fread(&nid, sizeof(int), 1, f) == 1 &&
        count >= 0 && count <= MAX_ITEMS &&
        fread(inventory, sizeof(Item), (size_t) count, f) == (size_t) count) {
        item_count = count;
        next_id = nid;
        for (int i = 0; i < item_count; i++)
            inventory[i].name[sizeof(inventory[i].name) - 1] = '\0';
    } else {
        fprintf(stderr, "Warning: %s is corrupt, starting empty\n", DATA_FILE);
        item_count = 0;
        next_id = 1001;
    }
    fclose(f);
}

// Writes a product name as a quoted CSV field, escaping quotes and blocking spreadsheet formulas.
static void csv_write_name(FILE *f, const char *s) {
    fputc('"', f);

    if (*s == '=' || *s == '+' || *s == '-' || *s == '@') fputc('\'', f);
    for (; *s; s++) {
        if (*s == '"') fputc('"', f);
        fputc(*s, f);
    }
    fputc('"', f);
}

// handles export csv event
int export_csv(void) {
    FILE *f = fopen(CSV_FILE, "wb");
    if (!f) return 0;
    fprintf(f, "\xEF\xBB\xBF");
    fprintf(f, "ID,Name,Quantity,Min Stock,Unit Price,Total Value,Status\r\n");
    double grand = 0;
    for (int i = 0; i < item_count; i++) {
        Item *it = &inventory[i];
        double total = (double) it->qty * it->price;
        grand += total;
        fprintf(f, "%d,", it->id);
        csv_write_name(f, it->name);
        fprintf(f, ",%d,%d,%.2f,%.2f,%s\r\n", it->qty, it->min_stock, it->price, total,
                it->qty == 0 ? "Out of Stock" : (it->qty <= it->min_stock ? "Low Stock" : "OK"));
    }
    fprintf(f, ",TOTAL INVENTORY VALUE,,,,%.2f,\r\n", grand);
    fclose(f);
    return 1;
}

// json file formatting mongoose
static size_t json_escape(char *dst, size_t cap, const char *src) {
    size_t n = 0;
    for (; *src && n + 7 < cap; src++) {
        unsigned char ch = (unsigned char) *src;
        if (ch == '"' || ch == '\\') { dst[n++] = '\\'; dst[n++] = (char) ch; }
        else if (ch < 0x20) n += (size_t) snprintf(dst + n, cap - n, "\\u%04x", ch);
        else dst[n++] = (char) ch;
    }
    dst[n] = '\0';
    return n;
}

// Convert to json
static int item_to_json(char *buf, size_t cap, const Item *it) {
    char esc[64 * 6 + 1];
    json_escape(esc, sizeof(esc), it->name);
    return snprintf(buf, cap,
                    "{\"id\":%d,\"name\":\"%s\",\"qty\":%d,\"min_stock\":%d,\"price\":%.2f}",
                    it->id, esc, it->qty, it->min_stock, it->price);
}

// Handles errors while establishing connection
static void reply_error(struct mg_connection *c, int code, const char *msg) {
    mg_http_reply(c, code, "Content-Type: application/json\r\n", "{\"error\":\"%s\"}\n", msg);
}

// Returns JSON response with HTTP status code.
static void reply_item(struct mg_connection *c, int code, const Item *it) {
    char buf[512];
    item_to_json(buf, sizeof(buf), it);
    mg_http_reply(c, code, "Content-Type: application/json\r\n", "%s\n", buf);
}

// Removes leading and trailing whitespace from a string in place.
static void trim(char *s) {
    size_t len = strlen(s), start = 0;
    while (len > 0 && (s[len - 1] == ' ' || s[len - 1] == '\t' || s[len - 1] == '\r' || s[len - 1] == '\n')) s[--len] = '\0';
    while (s[start] == ' ' || s[start] == '\t') start++;
    if (start) memmove(s, s + start, strlen(s + start) + 1);
}

//returns all inventory items as a JSON array.
static void api_list(struct mg_connection *c) {
    size_t cap = (size_t) item_count * 512 + 64;
    char *buf = (char *) malloc(cap);
    if (!buf) { reply_error(c, 500, "Out of memory"); return; }
    size_t n = 0;
    buf[n++] = '[';
    for (int i = 0; i < item_count; i++) {
        if (i) buf[n++] = ',';
        n += (size_t) item_to_json(buf + n, cap - n, &inventory[i]);
    }
    buf[n++] = ']';
    buf[n] = '\0';
    mg_http_reply(c, 200, "Content-Type: application/json\r\n", "%s", buf);
    free(buf);
}

// validates the JSON body(that i get on req), assigns the next id, appends the item and saves.
static void api_add(struct mg_connection *c, struct mg_str body) {
    if (item_count >= MAX_ITEMS) { reply_error(c, 507, "Inventory is full (1000 items max)"); return; }
    char *name = mg_json_get_str(body, "$.name");
    double qty = 0, min_stock = 0, price = 0;
    int ok = name != NULL;
    if (ok) {
        trim(name);
        ok = name[0] != '\0';
    }
    if (!ok) { free(name); reply_error(c, 400, "Product name is required"); return; }
    if (!mg_json_get_num(body, "$.qty", &qty) || !mg_json_get_num(body, "$.min_stock", &min_stock) ||
        !mg_json_get_num(body, "$.price", &price)) {
        free(name); reply_error(c, 400, "qty, min_stock and price must be numbers"); return;
    }
    if (qty < 0 || min_stock < 0 || price < 0 || qty > 1e9 || min_stock > 1e9 || price > 1e9) {
        free(name); reply_error(c, 400, "Values must be non-negative and reasonable"); return;
    }
    Item *it = &inventory[item_count];
    memset(it, 0, sizeof(*it));
    it->id = next_id++;
    strncpy(it->name, name, sizeof(it->name) - 1);
    it->qty = (int) qty;
    it->min_stock = (int) min_stock;
    it->price = (float) price;
    item_count++;
    free(name);
    save_data();
    reply_item(c, 201, it);
}

// (PUT req) find the item using binary search and update
static void api_update(struct mg_connection *c, int id, struct mg_str body) {
    int idx = binary_search_id(id);
    if (idx < 0) { reply_error(c, 404, "Item not found"); return; }
    Item tmp = inventory[idx];
    char *name = mg_json_get_str(body, "$.name");
    if (name) {
        trim(name);
        if (name[0] == '\0') { free(name); reply_error(c, 400, "Product name cannot be empty"); return; }
        memset(tmp.name, 0, sizeof(tmp.name));
        strncpy(tmp.name, name, sizeof(tmp.name) - 1);
        free(name);
    }
    double v;
    if (mg_json_get_num(body, "$.qty", &v)) { if (v < 0 || v > 1e9) { reply_error(c, 400, "Invalid qty"); return; } tmp.qty = (int) v; }
    if (mg_json_get_num(body, "$.min_stock", &v)) { if (v < 0 || v > 1e9) { reply_error(c, 400, "Invalid min_stock"); return; } tmp.min_stock = (int) v; }
    if (mg_json_get_num(body, "$.price", &v)) { if (v < 0 || v > 1e9) { reply_error(c, 400, "Invalid price"); return; } tmp.price = (float) v; }
    inventory[idx] = tmp;
    save_data();
    reply_item(c, 200, &inventory[idx]);
}

// POST req adds or removes stock
static void api_stock(struct mg_connection *c, int id, struct mg_str body) {
    int idx = binary_search_id(id);
    if (idx < 0) { reply_error(c, 404, "Item not found"); return; }
    double delta;
    if (!mg_json_get_num(body, "$.delta", &delta) || delta != (double) (long) delta || delta < -1e9 || delta > 1e9) {
        reply_error(c, 400, "delta must be a whole number"); return;
    }
    long newq = (long) inventory[idx].qty + (long) delta;
    if (newq < 0) { reply_error(c, 409, "Not enough stock to remove that quantity"); return; }
    if (newq > 1000000000L) { reply_error(c, 400, "Quantity too large"); return; }
    inventory[idx].qty = (int) newq;
    save_data();
    reply_item(c, 200, &inventory[idx]);
}

// DELETE 
//keep the array sorted.
static void api_delete(struct mg_connection *c, int id) {
    int idx = binary_search_id(id);
    if (idx < 0) { reply_error(c, 404, "Item not found"); return; }
    for (int i = idx; i < item_count - 1; i++) inventory[i] = inventory[i + 1];
    item_count--;
    save_data();
    mg_http_reply(c, 200, "Content-Type: application/json\r\n", "{\"deleted\":%d}\n", id);
}

// Returns true if the request uses the given HTTP method (e.g. "GET").
static int is_method(struct mg_http_message *hm, const char *m) {
    return mg_strcmp(hm->method, mg_str(m)) == 0;
}

// Extracts the item id from /api/items/<id>[/stock]; returns -1 if the path is invalid.
static int parse_item_path(struct mg_http_message *hm, int *has_stock_suffix) {
    const char *prefix = "/api/items/";
    size_t plen = strlen(prefix);
    *has_stock_suffix = 0;
    if (hm->uri.len <= plen || strncmp(hm->uri.buf, prefix, plen) != 0) return -1;
    char tmp[32];
    size_t rest = hm->uri.len - plen;
    if (rest >= sizeof(tmp)) return -1;
    memcpy(tmp, hm->uri.buf + plen, rest);
    tmp[rest] = '\0';
    char *slash = strchr(tmp, '/');
    if (slash) {
        if (strcmp(slash, "/stock") != 0) return -1;
        *slash = '\0';
        *has_stock_suffix = 1;
    }
    char *end;
    long id = strtol(tmp, &end, 10);
    if (*tmp == '\0' || *end != '\0' || id <= 0 || id > 2000000000L) return -1;
    return (int) id;
}

// Main HTTP event and request handler CSV and API
static void handle(struct mg_connection *c, int ev, void *ev_data) {
    if (ev != MG_EV_HTTP_MSG) return;
    struct mg_http_message *hm = (struct mg_http_message *) ev_data;

    if (mg_strcmp(hm->uri, mg_str("/")) == 0 || mg_strcmp(hm->uri, mg_str("/index.html")) == 0) {
        struct mg_http_serve_opts opts = {0};
        mg_http_serve_file(c, hm, "index.html", &opts);
    } else if (mg_strcmp(hm->uri, mg_str("/api/items")) == 0) {
        if (is_method(hm, "GET")) api_list(c);
        else if (is_method(hm, "POST")) api_add(c, hm->body);
        else reply_error(c, 405, "Method not allowed");
    } else if (mg_strcmp(hm->uri, mg_str("/api/export")) == 0) {
        if (!is_method(hm, "GET")) { reply_error(c, 405, "Method not allowed"); return; }
        if (!export_csv()) { reply_error(c, 500, "Could not write " CSV_FILE); return; }
        struct mg_http_serve_opts opts = {0};
        opts.extra_headers = "Content-Disposition: attachment; filename=\"" CSV_FILE "\"\r\n";
        opts.mime_types = "csv=text/csv";
        mg_http_serve_file(c, hm, CSV_FILE, &opts);
    } else if (hm->uri.len > 11 && strncmp(hm->uri.buf, "/api/items/", 11) == 0) {
        int stock;
        int id = parse_item_path(hm, &stock);
        if (id < 0) { reply_error(c, 400, "Invalid item id"); return; }
        if (stock) {
            if (is_method(hm, "POST")) api_stock(c, id, hm->body);
            else reply_error(c, 405, "Method not allowed");
        } else if (is_method(hm, "GET")) {
            int idx = binary_search_id(id);
            if (idx < 0) reply_error(c, 404, "Item not found");
            else reply_item(c, 200, &inventory[idx]);
        } else if (is_method(hm, "PUT")) api_update(c, id, hm->body);
        else if (is_method(hm, "DELETE")) api_delete(c, id);
        else reply_error(c, 405, "Method not allowed");
    } else {
        reply_error(c, 404, "Not found");
    }
}

// Loads saved data,Mongoose server on port 8000
int main(void) {
    load_data();
    struct mg_mgr mgr;
    mg_mgr_init(&mgr);
    if (mg_http_listen(&mgr, LISTEN_URL, handle, NULL) == NULL) {
        fprintf(stderr, "Failed to listen on %s (is port 8000 already in use?)\n", LISTEN_URL);
        mg_mgr_free(&mgr);
        return 1;
    }
    printf("Inventory server running at http://localhost:8000  (%d items loaded)\n", item_count);
    printf("Press Ctrl+C to stop.\n");
    for (;;) mg_mgr_poll(&mgr, 100);
    mg_mgr_free(&mgr);
    return 0;
}
