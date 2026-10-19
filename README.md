# Inventory Management System

A web-based inventory manager with a **C backend** (Mongoose embedded HTTP server) and a single-page HTML + Tailwind CSS** dashboard. Built as a Data Structures & Algorithms lab project.

The core idea: item IDs are auto-generated in increasing order, so the item array is **always sorted by ID** with no sorting step. Every lookup, edit, stock change and delete therefore uses **binary search** in O(log N) instead of a linear scan.

## Features

- Add, edit, delete and look up products
- Quick stock adjustment (+1 / -1 or a custom amount); stock can never go below zero
- Low-stock alert banner and status badges (In stock / Low stock / Out of stock)
- Dashboard cards: total products, total units, total inventory value, low-stock count
- Live search by ID or name
- Data persists across restarts (binary file)
- One-click CSV export that opens in Excel
- Input validation on the server for every request

## Tech Stack

| Layer | Technology |
|---|---|
| Backend | C, [Mongoose](https://github.com/cesanta/mongoose) embedded HTTP server |
| Frontend | HTML5, Tailwind CSS (CDN), vanilla JavaScript (`fetch()`) |
| Storage | Binary file `inventory.dat` (`fwrite` / `fread`) |
| Reporting | CSV file `inventory_report.csv` (`fprintf`) |

## Project Structure

```
.
├── main.c         # HTTP server, API handlers, binary search, file I/O
├── mongoose.c     # Mongoose library source
├── mongoose.h     # Mongoose library header
└── index.html     # Dashboard UI
```

`inventory.dat` and `inventory_report.csv` are created automatically at runtime.

## How It Works

```
Browser (HTML/JS)  --fetch()-->  C server (Mongoose)  -->  inventory[] array  -->  inventory.dat
   shows data                    handles API requests       the real data           saved on disk
```

The browser is only a client. All data, validation rules, algorithms and file handling live in the C program.

### Data structure

```c
#define MAX_ITEMS 1000

typedef struct {
    int id;             // Auto-generated ID (1001, 1002, 1003...)
    char name[64];      // Product name
    int qty;            // Current stock quantity
    int min_stock;      // Low-stock alert threshold
    float price;        // Unit price
} Item;

Item inventory[MAX_ITEMS];
int item_count = 0;
int next_id = 1001;
```

New items are always appended with `id = next_id++`, so the array stays sorted by `id`. IDs are never reused after a deletion.

### Time complexity (N = number of items)

| Operation | Complexity |
|---|---|
| Find item by ID (`binary_search_id`) | O(log N) |
| Add item | O(1) to append |
| Edit item / change stock | O(log N) to find |
| Delete item | O(log N) to find + O(N) to shift later items left |
| List all, save to file, load from file, CSV export | O(N) |
| Name search in the browser | O(N x L), a linear filter on the client |

Every change rewrites `inventory.dat`, which is O(N). Space usage is O(MAX_ITEMS), a fixed array.

## API

| Method | Endpoint | Description |
|---|---|---|
| `GET` | `/api/items` | List all items |
| `POST` | `/api/items` | Add an item |
| `GET` | `/api/items/{id}` | Look up one item (binary search) |
| `PUT` | `/api/items/{id}` | Edit an item |
| `DELETE` | `/api/items/{id}` | Delete an item |
| `POST` | `/api/items/{id}/stock` | Change stock by `{"delta": n}` |
| `GET` | `/api/export` | Write and download the CSV report |


## Data Files

| File | Purpose |
|---|---|
| `inventory.dat` | Binary persistence. Stores `item_count`, `next_id` and the whole array. Written after every change and loaded at startup. |
| `inventory_report.csv` | Human-readable report for Excel: one row per item, a status column, total value per item and a grand total. Written only on export. |

## Security Notes

- The server listens on `127.0.0.1` only, so it is not reachable from other machines.
- Every request is validated in C: name required, no negative values, stock cannot go below zero, maximum of 1000 items.
- CSV export prefixes names starting with `=`, `+`, `-` or `@` to prevent spreadsheet formula injection.
- The UI escapes HTML in product names.

## Limitations and Future Work

- The whole data file is rewritten on every change; appending changes or using a database would scale better
- Fixed capacity of 1000 items (could use a dynamic array with `realloc`)
- Single-threaded server with no authentication
- Prices are stored as `float`
- Name search is linear; a hash map or trie would speed it up

## License and Credits

Built on [Mongoose](https://github.com/cesanta/mongoose) by Cesanta, which is distributed under its own license terms (open source GPLv2 or a commercial license).
