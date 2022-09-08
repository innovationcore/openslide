#include <config.h>
#include <glib.h>
#include <string.h>
#include <tiffio.h>
#include "isyntax.h"
#include "font8x8_basic.h" // From https://github.com/dhepper/font8x8/blob/8e279d2d864e79128e96188a6b9526cfa3fbfef9/font8x8_basic.h

// This header "poisons" some functions, so must be included after system headers that use the poisoned functions (eg fclose in wchar.h).
#include "openslide-private.h"

#define LOG(msg, ...) console_print(msg, ##__VA_ARGS__)
#define LOG_VAR(fmt, var) console_print("%s: %s=" fmt "\n", __FUNCTION__, #var, var)

static const struct _openslide_ops philips_isyntax_ops;

typedef struct philips_isyntax_level {
    struct _openslide_level base;
    isyntax_level_t* isyntax_level;
    struct _openslide_grid *grid;
} philips_isyntax_level;

typedef struct philips_isyntax_cache_t {
    isyntax_tile_list_t cache_list;
    GMutex mutex;
    // int refcount;
    int target_cache_size;
    block_allocator_t ll_coeff_block_allocator;
    block_allocator_t h_coeff_block_allocator;
    int allocator_block_width;
    int allocator_block_height;
} philips_isyntax_cache_t;

typedef struct philips_isyntax_t {
    isyntax_t* isyntax;
    philips_isyntax_cache_t* cache;
} philips_isyntax_t;

// Global cache, shared between all opened files (if enabled). Thread-safe initialization in open().
philips_isyntax_cache_t* philips_isyntax_global_cache_ptr = NULL;

void draw_horiz_line(uint32_t* tile_pixels, i32 tile_width, i32 y, i32 start, i32 end, uint32_t color) {
    for (int x = start; x < end; ++x) {
        tile_pixels[y*tile_width + x] = color;
    }
}

void draw_vert_line(uint32_t* tile_pixels, i32 tile_width, i32 x, i32 start, i32 end, uint32_t color) {
    for (int y = start; y < end; ++y) {
        tile_pixels[y*tile_width + x] = color;
    }
}

void draw_text(uint32_t* tile_pixels, i32 tile_width, i32 x_pos, i32 y_pos, uint32_t color, const char* text) {
    const int font_size = 8;
    for (char* ch = text; *ch != 0; ++ch) {
        for (int y = 0; y < font_size; ++y) {
            uint8_t bit_line = font8x8_basic[*ch][y];
            for (int x = 0; x < font_size; ++x) {
                if (bit_line & (1u << x)) {
                    tile_pixels[(y + y_pos) * tile_width + x + x_pos] = color;
                }
            }
        }
        x_pos += font_size;
    }
}

void annotate_tile(uint32_t* tile_pixels, i32 scale, i32 tile_col, i32 tile_row, i32 tile_width, i32 tile_height) {
#define IS_DEBUG_ANNOTATE_TILE false
#if IS_DEBUG_ANNOTATE_TILE
    // OpenCV in C is hard... the core_c.h includes types_c.h which includes cvdef.h which is c++.
    // But we don't need much. Axis-aligned lines, and some simple text.
    int pad = 1;
    uint32_t color = 0xff0000ff; // ARGB
    draw_horiz_line(tile_pixels, tile_width, /*y=*/pad, /*start=*/pad, /*end=*/tile_width-pad, color);
    draw_horiz_line(tile_pixels, tile_width, /*y=*/tile_height-pad, /*start=*/pad, /*end=*/tile_width-pad, color);

    draw_vert_line(tile_pixels, tile_width, /*x=*/pad, /*start=*/pad, /*end=*/tile_height-pad, color);
    draw_vert_line(tile_pixels, tile_width, /*x=*/tile_width-pad, /*start=*/pad, /*end=*/tile_height-pad, color);

    char buf[128];
    sprintf(buf, "x=%d,y=%d,s=%d", tile_row, tile_col, scale);
    draw_text(tile_pixels, tile_width, 10, 10, color, buf);
#else
    // Intentionally empty, the compiler should optimize the call away.
#endif
}

static bool philips_isyntax_detect(
        const char *filename,
        struct _openslide_tifflike *tl G_GNUC_UNUSED,
        GError **err G_GNUC_UNUSED) {
    LOG("got filename %s", filename);
    LOG_VAR("%p", tl);
    // reject TIFFs
    if (tl) {
        g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                    "Is a TIFF file");
        return false;
    }

    g_autoptr(_openslide_file) f = _openslide_fopen(filename, err);
    if (f == NULL) {
        LOG("Failed to open file");
        return false;
    }

    const int num_chars_to_read = 256;
    g_autofree char *buf = g_malloc(num_chars_to_read);
    size_t num_read = _openslide_fread(f, buf, num_chars_to_read-1);
    buf[num_chars_to_read-1] = 0;
    LOG_VAR("%d", num_read);
    LOG_VAR("%s", buf);

    // TODO(avirodov): probably a more robust XML parsing is needed.
    if (strstr(buf, "<DataObject ObjectType=\"DPUfsImport\">") != NULL) {
        LOG("got isyntax.");
        return true;
    }

    LOG("not isyntax.");
    return false;
}

static void tile_list_init(isyntax_tile_list_t* list, const char* dbg_name) {
    list->head = NULL;
    list->tail = NULL;
    list->count = 0;
    list->dbg_name = dbg_name;
}

static void tile_list_remove(isyntax_tile_list_t* list, isyntax_tile_t* tile) {
    if (!tile->cache_next && !tile->cache_prev && !(list->head == tile) && !(list->tail == tile)) {
        // Not part of any list.
        return;
    }
    if (list->head == tile) {
        list->head = tile->cache_next;
    }
    if (list->tail == tile) {
        list->tail = tile->cache_prev;
    }
    if (tile->cache_prev) {
        tile->cache_prev->cache_next = tile->cache_next;
    }
    if (tile->cache_next) {
        tile->cache_next->cache_prev = tile->cache_prev;
    }
    // Here we assume that the tile is part of this list, but we don't check (O(n)).
    tile->cache_next = NULL;
    tile->cache_prev = NULL;
    list->count--;
}

static void tile_list_insert_first(isyntax_tile_list_t* list, isyntax_tile_t* tile) {
    // printf("### tile_list_insert_first %s scale=%d x=%d y=%d\n", list->dbg_name, tile->dbg_tile_scale, tile->dbg_tile_x, tile->dbg_tile_y);
    ASSERT(tile->cache_next == NULL && tile->cache_prev == NULL);
    if (list->head == NULL) {
        list->head = tile;
        list->tail = tile;
    } else {
        list->head->cache_prev = tile;
        tile->cache_next = list->head;
        list->head = tile;
    }
    list->count++;
}

static void tile_list_insert_list_first(isyntax_tile_list_t* target_list, isyntax_tile_list_t* source_list) {
    if (source_list->head == NULL && source_list->tail == NULL) {
        return;
    }

    source_list->tail->cache_next = target_list->head;
    if (target_list->head) {
        target_list->head->cache_prev = source_list->tail;
    }

    target_list->head = source_list->head;
    if (target_list->tail == NULL) {
        target_list->tail = source_list->tail;
    }
    target_list->count += source_list->count;
    source_list->head = NULL;
    source_list->tail = NULL;
    source_list->count = 0;
}

#define ITERATE_TILE_LIST(_iter, _list) \
    isyntax_tile_t* _iter = _list.head; _iter; _iter = _iter->cache_next


void isyntax_openslide_load_tile_coefficients_ll_or_h(philips_isyntax_cache_t* cache, isyntax_t* isyntax, isyntax_tile_t* tile, int codeblock_index, bool is_ll) {
    isyntax_image_t* wsi = &isyntax->images[isyntax->wsi_image_index];
    isyntax_data_chunk_t* chunk = &wsi->data_chunks[tile->data_chunk_index];

    for (int color = 0; color < 3; ++color) {
        isyntax_codeblock_t* codeblock = &wsi->codeblocks[codeblock_index + color * chunk->codeblock_count_per_color];
        ASSERT(codeblock->coefficient == (is_ll ? 0 : 1)); // LL coefficient codeblock for this tile.
        ASSERT(codeblock->color_component == color);
        ASSERT(codeblock->scale == tile->dbg_tile_scale);
        if (is_ll) {
            tile->color_channels[color].coeff_ll = (icoeff_t *) block_alloc(&cache->ll_coeff_block_allocator);
        } else {
            tile->color_channels[color].coeff_h = (icoeff_t *) block_alloc(&cache->h_coeff_block_allocator);
        }
        // TODO(avirodov): fancy allocators, for multiple sequential blocks (aka chunk). Or let OS do the caching.
        u8* codeblock_data = malloc(codeblock->block_size);
        size_t bytes_read = file_handle_read_at_offset(codeblock_data, isyntax->file_handle,
                                                       codeblock->block_data_offset, codeblock->block_size);
        if (!(bytes_read > 0)) {
            console_print_error("Error: could not read iSyntax data at offset %lld (read size %lld)\n", offset0, read_size);
        }

        isyntax_hulsken_decompress(codeblock_data, codeblock->block_size,
                                   isyntax->block_width, isyntax->block_height,
                                   codeblock->coefficient, 1,
                                   is_ll ? tile->color_channels[color].coeff_ll : tile->color_channels[color].coeff_h);
        free(codeblock_data);
    }

    if (is_ll) {
        tile->has_ll = true;
    } else {
        tile->has_h = true;
    }
}

void isyntax_openslide_load_tile_coefficients(philips_isyntax_cache_t* cache, isyntax_t* isyntax, isyntax_tile_t* tile) {
    isyntax_image_t* wsi = &isyntax->images[isyntax->wsi_image_index];

    if (!tile->exists) {
        return;
    }

    // Load LL codeblocks here only for top-level tiles. For other levels, the LL coefficients are computed from parent
    // tiles later on.
    if (!tile->has_ll && tile->dbg_tile_scale == wsi->max_scale) {
        isyntax_openslide_load_tile_coefficients_ll_or_h(cache, isyntax, tile, /*codeblock_index=*/tile->codeblock_index, /*is_ll=*/true);
    }

    if (!tile->has_h) {
        ASSERT(tile->exists);
        isyntax_data_chunk_t* chunk = wsi->data_chunks + tile->data_chunk_index;

        i32 scale_in_chunk = chunk->scale - tile->dbg_tile_scale;
        ASSERT(scale_in_chunk >= 0 && scale_in_chunk < 3);
        i32 codeblock_index_in_chunk = 0;
        if (scale_in_chunk == 0) {
            codeblock_index_in_chunk = 0;
        } else if (scale_in_chunk == 1) {
            codeblock_index_in_chunk = 1 + (tile->dbg_tile_y % 2) * 2 + (tile->dbg_tile_x % 2);
        } else if (scale_in_chunk == 2) {
            codeblock_index_in_chunk = 5 + (tile->dbg_tile_y % 4) * 4 + (tile->dbg_tile_x % 4);
        } else {
            panic();
        }

        isyntax_openslide_load_tile_coefficients_ll_or_h(cache, isyntax, tile, /*codeblock_index=*/tile->codeblock_chunk_index + codeblock_index_in_chunk, /*is_ll=*/false);
    }
}

typedef union isyntax_tile_children_t {
    struct {
        isyntax_tile_t *child_top_left;
        isyntax_tile_t *child_top_right;
        isyntax_tile_t *child_bottom_left;
        isyntax_tile_t *child_bottom_right;
    };
    isyntax_tile_t* as_array[4];
} isyntax_tile_children_t;

isyntax_tile_children_t isyntax_openslide_compute_children(isyntax_t* isyntax, isyntax_tile_t* tile) {
    isyntax_tile_children_t result;
    isyntax_image_t* wsi = &isyntax->images[isyntax->wsi_image_index];
    ASSERT(tile->dbg_tile_scale > 0);
    isyntax_level_t *next_level = &wsi->levels[tile->dbg_tile_scale - 1];
    result.child_top_left = next_level->tiles + (tile->dbg_tile_y * 2) * next_level->width_in_tiles + (tile->dbg_tile_x * 2);
    result.child_top_right = result.child_top_left + 1;
    result.child_bottom_left = result.child_top_left + next_level->width_in_tiles;
    result.child_bottom_right = result.child_bottom_left + 1;
    return result;
}


uint32_t* isyntax_openslide_idwt(philips_isyntax_cache_t* cache, isyntax_t* isyntax, isyntax_tile_t* tile, bool return_rgb) {
    if (tile->dbg_tile_scale == 0) {
        ASSERT(return_rgb); // Shouldn't be asking for idwt at level 0 if we're not going to use the result for pixels.
        return isyntax_load_tile(isyntax, &isyntax->images[isyntax->wsi_image_index],
                                 tile->dbg_tile_scale, tile->dbg_tile_x, tile->dbg_tile_y,
                                 &cache->ll_coeff_block_allocator, /*decode_rgb=*/true);
    }

    if (return_rgb) {
        // TODO(avirodov): if we want rgb from tile where idwt was done already, this could be cheaper if we store
        //  the lls in the tile. Currently need to recompute idwt.
        return isyntax_load_tile(isyntax, &isyntax->images[isyntax->wsi_image_index],
                                 tile->dbg_tile_scale, tile->dbg_tile_x, tile->dbg_tile_y,
                                 &cache->ll_coeff_block_allocator, /*decode_rgb=*/true);
    }

    // If all children have ll coefficients and we don't need the rgb pixels, no need to do the idwt.
    ASSERT(!return_rgb && tile->dbg_tile_scale > 0);
    isyntax_tile_children_t children = isyntax_openslide_compute_children(isyntax, tile);
    if (children.child_top_left->has_ll && children.child_top_right->has_ll &&
        children.child_bottom_left->has_ll && children.child_bottom_right->has_ll) {
            return NULL;
    }

    isyntax_load_tile(isyntax, &isyntax->images[isyntax->wsi_image_index],
                      tile->dbg_tile_scale, tile->dbg_tile_x, tile->dbg_tile_y,
                      &cache->ll_coeff_block_allocator, /*decode_rgb=*/false);
    return NULL;
}

void isyntax_make_tile_lists_add_parent_to_list(isyntax_t* isyntax, isyntax_tile_t* tile,
                                                isyntax_tile_list_t* idwt_list, isyntax_tile_list_t* cache_list) {
    isyntax_image_t* wsi = &isyntax->images[isyntax->wsi_image_index];
    int parent_tile_scale = tile->dbg_tile_scale + 1;
    if (parent_tile_scale > wsi->max_scale) {
        return;
    }

    int parent_tile_x = tile->dbg_tile_x / 2;
    int parent_tile_y = tile->dbg_tile_y / 2;
    isyntax_level_t* parent_level = &wsi->levels[parent_tile_scale];
    isyntax_tile_t* parent_tile = &parent_level->tiles[parent_level->width_in_tiles * parent_tile_y + parent_tile_x];
    if (parent_tile->exists && !parent_tile->cache_marked) {
        tile_list_remove(cache_list, parent_tile);
        parent_tile->cache_marked = true;
        tile_list_insert_first(idwt_list, parent_tile);
    }
}

void isyntax_make_tile_lists_add_children_to_list(isyntax_t* isyntax, isyntax_tile_t* tile,
                                                  isyntax_tile_list_t* children_list, isyntax_tile_list_t* cache_list) {
    isyntax_image_t* wsi = &isyntax->images[isyntax->wsi_image_index];
    if (tile->dbg_tile_scale > 0) {
        isyntax_tile_children_t children = isyntax_openslide_compute_children(isyntax, tile);
        for (int i = 0; i < 4; ++i) {
            if (!children.as_array[i]->cache_marked) {
                tile_list_remove(cache_list, children.as_array[i]);
                tile_list_insert_first(children_list, children.as_array[i]);
            }
        }
    }
}

void isyntax_make_tile_lists_by_scale(isyntax_t* isyntax, int start_scale,
                                      isyntax_tile_list_t* idwt_list,
                                      isyntax_tile_list_t* coeff_list,
                                      isyntax_tile_list_t* children_list,
                                      isyntax_tile_list_t* cache_list) {
    isyntax_image_t* wsi = &isyntax->images[isyntax->wsi_image_index];
    for (int scale = start_scale; scale <= wsi->max_scale; ++scale) {
        // Mark all neighbors of idwt tiles at this level as requiring coefficients.
        isyntax_level_t* level = &wsi->levels[scale];
        for (ITERATE_TILE_LIST(tile, (*idwt_list))) {
            if (tile->dbg_tile_scale == scale) {
                for (int y_offset = -1; y_offset <= 1; ++y_offset) {
                    for (int x_offset = -1; x_offset <= 1; ++ x_offset) {
                        int neighbor_tile_x =  tile->dbg_tile_x + x_offset;
                        int neighbor_tile_y = tile->dbg_tile_y + y_offset;
                        if (neighbor_tile_x < 0 || neighbor_tile_x >= level->width_in_tiles ||
                            neighbor_tile_y < 0 || neighbor_tile_y >= level->height_in_tiles) {
                            continue;
                        }

                        isyntax_tile_t* neighbor_tile = &level->tiles[level->width_in_tiles * neighbor_tile_y + neighbor_tile_x];
                        if (neighbor_tile->cache_marked || !neighbor_tile->exists) {
                            continue;
                        }

                        tile_list_remove(cache_list, neighbor_tile);
                        neighbor_tile->cache_marked = true;
                        tile_list_insert_first(coeff_list, neighbor_tile);
                    }
                }
            }
        }

        // Mark all parents of tiles at this level as requiring idwt. This way all tiles at this level will get their
        // ll coefficients.
        for (ITERATE_TILE_LIST(tile, (*idwt_list))) {
            if (tile->dbg_tile_scale == scale) {
                isyntax_make_tile_lists_add_parent_to_list(isyntax, tile, idwt_list, cache_list);
            }
        }
        for (ITERATE_TILE_LIST(tile, (*coeff_list))) {
            if (tile->dbg_tile_scale == scale) {
                isyntax_make_tile_lists_add_parent_to_list(isyntax, tile, idwt_list, cache_list);
            }
        }
    }

    // Add all children of idwt that were not yet handled. The children will have their ll coefficients written,
    // and so should be cache bumped.
    // TODO(avirodov): if we store the idwt result (ll of next level) in the tile instead of the children, this
    //  would be unnecessary. But I'm not sure this is bad either.
    for (ITERATE_TILE_LIST(tile, (*idwt_list))) {
        isyntax_make_tile_lists_add_children_to_list(isyntax, tile, children_list, cache_list);
    }
}

static uint32_t* isyntax_openslide_load_tile(philips_isyntax_cache_t* cache, isyntax_t* isyntax, int scale, int tile_x, int tile_y) {
    // TODO(avirodov): more granular locking (some notes below). This will require handling overlapping work, that is
    //  thread A needing tile 123 and started to load it, and thread B needing same tile 123 and needs to wait for A.
    g_autoptr(GMutexLocker) locker G_GNUC_UNUSED = g_mutex_locker_new(&cache->mutex);

    isyntax_image_t* wsi = &isyntax->images[isyntax->wsi_image_index];
    isyntax_level_t* level = &wsi->levels[scale];
    isyntax_tile_t *tile = &level->tiles[level->width_in_tiles * tile_y + tile_x];
    // printf("=== isyntax_openslide_load_tile scale=%d tile_x=%d tile_y=%d\n", scale, tile_x, tile_y);
    if (!tile->exists) {
        uint32_t* rgba = malloc(isyntax->tile_width * isyntax->tile_height * 4);
        memset(rgba, 0xff, isyntax->tile_width * isyntax->tile_height * 4);
        return rgba;
    }

    // Need 3 lists:
    // 1. idwt list - those tiles will have to perform an idwt for their children to get ll coeffs. Primary cache bump.
    // 2. coeff list - those tiles are neighbors and will need to have coefficients loaded. Secondary cache bump.
    // 3. children list - those tiles will have their ll coeffs loaded as a side effect. Tertiary cache bump.
    // Those lists must be disjoint, and sorted such that parents are closer to head than children.
    isyntax_tile_list_t idwt_list = {NULL, NULL, 0, "idwt_list"};
    isyntax_tile_list_t coeff_list = {NULL, NULL, 0, "coeff_list"};
    isyntax_tile_list_t children_list = {NULL, NULL, 0, "children_list"};

    // Lock.
    // Make a list of all dependent tiles (including the required one).
    // Mark all dependent tiles as "reserved" so that they are not evicted by other threads as we load them.
    // Unlock.
    {
        tile_list_remove(&cache->cache_list, tile);
        tile->cache_marked = true;
        tile_list_insert_first(&idwt_list, tile);
    }
    isyntax_make_tile_lists_by_scale(isyntax, scale, &idwt_list, &coeff_list, &children_list, &cache->cache_list);

    // Unmark visit status and reserve all nodes (todo later).
    for (ITERATE_TILE_LIST(tile, idwt_list))     { tile->cache_marked = false; /*printf("@@@ idwt_list tile scale=%d x=%d y=%d\n", tile->dbg_tile_scale, tile->dbg_tile_x, tile->dbg_tile_y);*/ }
    for (ITERATE_TILE_LIST(tile, coeff_list))    { tile->cache_marked = false; /*printf("@@@ coeff_list tile scale=%d x=%d y=%d\n", tile->dbg_tile_scale, tile->dbg_tile_x, tile->dbg_tile_y);*/ }
    for (ITERATE_TILE_LIST(tile, children_list)) { tile->cache_marked = false; /*printf("@@@ children_list tile scale=%d x=%d y=%d\n", tile->dbg_tile_scale, tile->dbg_tile_x, tile->dbg_tile_y);*/ }

    // IO+decode: For all dependent tiles, read and decode coefficients where missing (hh, and ll for top tiles).
    // Assuming lists are sorted parents first.
    // IDWT as needed, top to bottom. This should produce idwt for this tile as well, which should be last in idwt list.
    // YCoCb->RGB for this tile only.
    uint32_t* result = NULL;
    for (ITERATE_TILE_LIST(tile, coeff_list)) {
        isyntax_openslide_load_tile_coefficients(cache, isyntax, tile);
    }
    for (ITERATE_TILE_LIST(tile, idwt_list)) {
        isyntax_openslide_load_tile_coefficients(cache, isyntax, tile);
    }
    for (ITERATE_TILE_LIST(tile, idwt_list)) {
        if (tile == idwt_list.tail) {
            result = isyntax_openslide_idwt(cache, isyntax, tile, /*return_rgb=*/true);
        } else {
            isyntax_openslide_idwt(cache, isyntax, tile, /*return_rgb=*/false);
        }
    }

    // Lock.
    // Bump all the affected tiles in cache.
    // Unmark all dependent tiles as "referenced" so that they can be evicted.
    // Perform cache trim (possibly not every invocation).
    // Unlock.

    tile_list_insert_list_first(&cache->cache_list, &children_list);
    tile_list_insert_list_first(&cache->cache_list, &coeff_list);
    tile_list_insert_list_first(&cache->cache_list, &idwt_list);

    // Cache trim. Since we have the result already, it is possible that tiles from this run will be trimmed here
    // if cache is small or work happened on other threads.
    // TODO(avirodov): later will need to skip tiles that are reserved by other threads.
    while (cache->cache_list.count > cache->target_cache_size) {
        isyntax_tile_t* tile = cache->cache_list.tail;
        tile_list_remove(&cache->cache_list, tile);
        for (int i = 0; i < 3; ++i) {
            if (tile->has_ll) {
                block_free(&cache->ll_coeff_block_allocator, tile->color_channels[i].coeff_ll);
                tile->color_channels[i].coeff_ll = NULL;
            }
            if (tile->has_h) {
                block_free(&cache->h_coeff_block_allocator, tile->color_channels[i].coeff_h);
                tile->color_channels[i].coeff_h = NULL;
            }
        }
        tile->has_ll = false;
        tile->has_h = false;
    }

    return result;
}

static bool philips_isyntax_read_tile(
        openslide_t *osr,
        cairo_t *cr,
        struct _openslide_level *osr_level,
        int64_t tile_col, int64_t tile_row,
        void *arg G_GNUC_UNUSED,
        GError **err) {
    philips_isyntax_t *data = osr->data;
    isyntax_t* isyntax = data->isyntax;

    philips_isyntax_level* pi_level = (philips_isyntax_level*)osr_level;
    isyntax_image_t* wsi_image = &isyntax->images[isyntax->wsi_image_index];

    // LOG("level=%d tile_col=%ld tile_row=%ld", pi_level->level_idx, tile_col, tile_row);
    // tile size
    int64_t tw = isyntax->tile_width;
    int64_t th = isyntax->tile_height;

    // Openslide cache
    g_autoptr(_openslide_cache_entry) cache_entry = NULL;
    uint32_t *tiledata = _openslide_cache_get(osr->cache,
                                              pi_level, tile_col, tile_row,
                                              &cache_entry);
    if (!tiledata) {
        tiledata = isyntax_openslide_load_tile(data->cache, isyntax, pi_level->isyntax_level->scale, tile_col, tile_row);
        annotate_tile(tiledata, pi_level->isyntax_level->scale, tile_col, tile_row, tw, th);

        _openslide_cache_put(osr->cache, pi_level, tile_col, tile_row,
                             tiledata, tw * th * 4,
                             &cache_entry);
    }

    // draw it
    g_autoptr(cairo_surface_t) surface =
            cairo_image_surface_create_for_data((unsigned char *) tiledata,
                                                CAIRO_FORMAT_ARGB32,
                                                tw, th, tw * 4);
    cairo_set_source_surface(cr, surface, 0, 0);
    cairo_paint(cr);

    return true;
}

static void add_float_property(openslide_t *osr, const char* property_name, float value) {
    g_hash_table_insert(osr->properties, g_strdup(property_name),
                        _openslide_format_double(value));
}

static philips_isyntax_cache_t* philips_isyntax_make_cache(const char* dbg_name, int cache_size, int block_width, int block_height) {
    philips_isyntax_cache_t* cache_ptr = malloc(sizeof(philips_isyntax_cache_t));
    tile_list_init(&cache_ptr->cache_list, dbg_name);
    cache_ptr->target_cache_size = cache_size;
    g_mutex_init(&cache_ptr->mutex);

    cache_ptr->allocator_block_width = block_width;
    cache_ptr->allocator_block_height = block_height;
    size_t ll_coeff_block_size = block_width * block_height * sizeof(icoeff_t);
    size_t block_allocator_maximum_capacity_in_blocks = GIGABYTES(32) / ll_coeff_block_size;
    size_t ll_coeff_block_allocator_capacity_in_blocks = block_allocator_maximum_capacity_in_blocks / 4;
    size_t h_coeff_block_size = ll_coeff_block_size * 3;
    size_t h_coeff_block_allocator_capacity_in_blocks = ll_coeff_block_allocator_capacity_in_blocks * 3;
    cache_ptr->ll_coeff_block_allocator = block_allocator_create(ll_coeff_block_size, ll_coeff_block_allocator_capacity_in_blocks, MEGABYTES(256));
    cache_ptr->h_coeff_block_allocator = block_allocator_create(h_coeff_block_size, h_coeff_block_allocator_capacity_in_blocks, MEGABYTES(256));
    return cache_ptr;
}

static bool philips_isyntax_open(
        openslide_t *osr,
        const char *filename,
        struct _openslide_tifflike *tl G_GNUC_UNUSED,
        struct _openslide_hash *quickhash1 G_GNUC_UNUSED,
        GError **err) {
    // Do not allow multithreading in opening.
    static GStaticMutex static_open_mutex = G_STATIC_MUTEX_INIT;
    g_static_mutex_lock(&static_open_mutex);
    static bool threadmemory_initialized = false;
    if (!threadmemory_initialized) {
        get_system_info(/*verbose=*/true);
        init_thread_memory(0);
        threadmemory_initialized = true;
    }
    g_static_mutex_unlock(&static_open_mutex);
    LOG("Opening file %s", filename);

    philips_isyntax_t* data = malloc(sizeof(philips_isyntax_t));
    data->isyntax = malloc(sizeof(isyntax_t));
    memset(data->isyntax, 0, sizeof(isyntax_t));

    osr->data = data;
    bool open_result = isyntax_open(data->isyntax, filename);
    LOG_VAR("%d", (int)open_result);
    LOG_VAR("%d", data->image_count);
    if (!open_result) {
        free(data->isyntax);
        free(data);
        g_prefix_error(err, "Can't open file.");
        return false;
    }

    // Initialize the cache (global, if requested).
    bool is_global_cache = true;
    int cache_size = 2000;
    const char* str_is_global_cache = g_environ_getenv(g_get_environ(), "OPENSLIDE_ISYNTAX_GLOBAL_CACHE");
    const char* str_cache_size = g_environ_getenv(g_get_environ(), "OPENSLIDE_ISYNTAX_CACHE_SIZE");
    if (str_is_global_cache && *str_is_global_cache == '0') {
        is_global_cache = false;
    }
    if (str_cache_size) {
        cache_size = atoi(str_cache_size);
    }
    printf("philips_isyntax_open is_global_cache=%d cache_size=%d\n", (int)is_global_cache, cache_size);
    if (is_global_cache) {
        g_static_mutex_lock(&static_open_mutex);
        if (philips_isyntax_global_cache_ptr == NULL) {
            // Note: this requires that all opened files have the same block size. If that is not true, we
            // will need to have allocator per size. Alternatively, implement allocator freeing after
            // all tiles have been freed, and track isyntax_t per tile so we can access allocator.
            philips_isyntax_global_cache_ptr = philips_isyntax_make_cache("global_cache_list", cache_size,
                                                                          data->isyntax->block_width,
                                                                          data->isyntax->block_height);
        }
        data->cache = philips_isyntax_global_cache_ptr;
        g_static_mutex_unlock(&static_open_mutex);
    } else {
        data->cache = philips_isyntax_make_cache("cache_list", cache_size,
                                                 data->isyntax->block_width, data->isyntax->block_height);
    }
    ASSERT(data->isyntax->block_width == data->cache->allocator_block_width);
    ASSERT(data->isyntax->block_height == data->cache->allocator_block_height);

    LOG_VAR("%d", data->is_mpp_known);
    if (data->isyntax->is_mpp_known) {
        LOG_VAR("%f", data->mpp_x);
        LOG_VAR("%f", data->mpp_y);
        add_float_property(osr, OPENSLIDE_PROPERTY_NAME_MPP_X, data->isyntax->mpp_x);
        add_float_property(osr, OPENSLIDE_PROPERTY_NAME_MPP_Y, data->isyntax->mpp_x);
        const float float_equals_tolerance = 1e-5;
        if (fabsf(data->isyntax->mpp_x - data->isyntax->mpp_y) < float_equals_tolerance) {
            // Compute objective power from microns-per-pixel, see e.g. table in "Scan Performance" here:
            // https://www.microscopesinternational.com/blog/20170928-whichobjective.aspx
            float objective_power = 10.0f / data->isyntax->mpp_x;
            LOG_VAR("%f", objective_power);
            add_float_property(osr, OPENSLIDE_PROPERTY_NAME_OBJECTIVE_POWER, objective_power);
        }
    }

    // Find wsi image. Extracting other images not supported. Assuming only one wsi.
    int wsi_image_idx = data->isyntax->wsi_image_index;
    LOG_VAR("%d", wsi_image_idx);
    g_assert(wsi_image_idx >= 0 && wsi_image_idx < data->isyntax->image_count);
    isyntax_image_t* wsi_image = &data->isyntax->images[wsi_image_idx];

    // Store openslide information about each level.
    isyntax_level_t* levels = wsi_image->levels;
    osr->levels = malloc(sizeof(philips_isyntax_level*) * wsi_image->level_count);
    osr->level_count = wsi_image->level_count;
    for (int i = 0; i < wsi_image->level_count; ++i) {
        philips_isyntax_level* level = malloc(sizeof(philips_isyntax_level));
        level->isyntax_level = &wsi_image->levels[i];
        level->base.downsample = levels[i].downsample_factor;
        level->base.w = levels[i].width_in_tiles * data->isyntax->tile_width;
        level->base.h = levels[i].height_in_tiles * data->isyntax->tile_height;
        level->base.tile_w = data->isyntax->tile_width;
        level->base.tile_h = data->isyntax->tile_height;
        osr->levels[i] = (struct _openslide_level*)level;
        level->grid = _openslide_grid_create_simple(
                osr,
                levels[i].width_in_tiles,
                levels[i].height_in_tiles,
                data->isyntax->tile_width,
                data->isyntax->tile_height,
                philips_isyntax_read_tile);

        // LOG_VAR("%d", data->images[wsi_image_idx].levels[i].scale);
        LOG_VAR("%d", i);
        LOG_VAR("%d", levels[i].scale);
        LOG_VAR("%d", levels[i].width_in_tiles);
        LOG_VAR("%d", levels[i].height_in_tiles);
        LOG_VAR("%f", levels[i].downsample_factor);
        LOG_VAR("%f", levels[i].um_per_pixel_x);
        LOG_VAR("%f", levels[i].um_per_pixel_y);
        LOG_VAR("%f", levels[i].x_tile_side_in_um);
        LOG_VAR("%f", levels[i].y_tile_side_in_um);
        LOG_VAR("%lld", levels[i].tile_count);
        LOG_VAR("%f", levels[i].origin_offset_in_pixels);
        LOG_VAR("%f", levels[i].origin_offset.x);
        LOG_VAR("%f", levels[i].origin_offset.y);
        LOG_VAR("%d", (int)levels[i].is_fully_loaded);
    }
    osr->ops = &philips_isyntax_ops;
    return true;
}

static bool philips_isyntax_paint_region(
        openslide_t *osr G_GNUC_UNUSED, cairo_t *cr,
        int64_t x, int64_t y,
        struct _openslide_level *osr_level,
        int32_t w, int32_t h,
        GError **err) {
    philips_isyntax_level* level = (philips_isyntax_level*)osr_level;

    // LOG("x=%ld y=%ld level=%d w=%d h=%d", x, y, level->level_idx, w, h);
    // Note: round is necessary to avoid producing resampled (and thus blurry) images on higher levels.
    return _openslide_grid_paint_region(level->grid, cr, NULL,
                                        round((x - level->isyntax_level->origin_offset_in_pixels) / level->base.downsample),
                                        round((y - level->isyntax_level->origin_offset_in_pixels) / level->base.downsample),
                                        osr_level, w, h,
                                        err);
}

static void philips_isyntax_destroy(openslide_t *osr) {
    philips_isyntax_t *data = osr->data;

    for (int i = 0; i < osr->level_count; ++i) {
        philips_isyntax_level* level = (philips_isyntax_level*)osr->levels[i];
        _openslide_grid_destroy(level->grid);
        free(level);
    }
    // Flush cache (especially if global).
    // TODO(avirodov): if we track for each tile (or cache entry) which isyntax_t* it came from, we can remove
    //  only those entries from global cache.
    if (data->cache == philips_isyntax_global_cache_ptr) {
        g_autoptr(GMutexLocker) locker G_GNUC_UNUSED =
                g_mutex_locker_new(&data->cache->mutex);
        while (data->cache->cache_list.tail) {
            tile_list_remove(&data->cache->cache_list, data->cache->cache_list.tail);
        }

    } else {
        // Not shared (for now sharing is either global or none).
        if (data->cache->ll_coeff_block_allocator.is_valid) {
            block_allocator_destroy(&data->cache->ll_coeff_block_allocator);
        }
        if (data->cache->h_coeff_block_allocator.is_valid) {
            block_allocator_destroy(&data->cache->h_coeff_block_allocator);
        }
        free(data->cache);
    }

    free(osr->levels);
    isyntax_destroy(data->isyntax);
    free(data->isyntax);
    free(data);
}

const struct _openslide_format _openslide_format_philips_isyntax = {
        .name = "philips-isyntax",
        .vendor = "philips-isyntax",
        .detect = philips_isyntax_detect,
        .open = philips_isyntax_open,
};

static const struct _openslide_ops philips_isyntax_ops = {
        .paint_region = philips_isyntax_paint_region,
        .destroy = philips_isyntax_destroy,
};
