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
#define IS_DEBUG_ANNOTATE_TILE true
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

void submit_tile_completed(
        void* userdata,
        void* tile_pixels,
        i32 scale,
        i32 tile_index,
        i32 tile_width G_GNUC_UNUSED,
        i32 tile_height G_GNUC_UNUSED) {
    openslide_t *osr = (openslide_t*)userdata;
    isyntax_t *isyntax = osr->data;
    philips_isyntax_level* pi_level = (philips_isyntax_level*)osr->levels[scale];

    i32 width = pi_level->isyntax_level->width_in_tiles;
    i32 tile_col = tile_index % width;
    i32 tile_row = tile_index / width;
    LOG("### level=%d tile_col=%d tile_row=%d", pi_level->isyntax_level->scale, tile_col, tile_row);
    // tile size
    int64_t tw = isyntax->tile_width;
    int64_t th = isyntax->tile_height;
    annotate_tile(tile_pixels, pi_level->isyntax_level->scale, tile_col, tile_row, tw, th);

    // cache
    g_autoptr(_openslide_cache_entry) cache_entry = NULL;

    _openslide_slice box = {
            .p = tile_pixels,
            .len = tw * th * 4
    };

    // clip, if necessary
    if (!_openslide_clip_tile(box.p,
                              tw, th,
                              pi_level->base.w - tile_col * tw,
                              pi_level->base.h - tile_row * th,
                              NULL)) {
        // TODO(avirodov): what happens here???
    }

    // put it in the cache
    tile_pixels = _openslide_slice_steal(&box);

    uint32_t *tiledata = _openslide_cache_get(osr->cache,
                                              pi_level, tile_col, tile_row,
                                              &cache_entry);
    if (!tiledata) {
        _openslide_cache_put(osr->cache, pi_level, tile_col, tile_row,
                             tile_pixels, tw * th * 4,
                             &cache_entry);
    } else {
        LOG("@@@ redundant tile level=%d tile_col=%d tile_row=%d", pi_level->isyntax_level->scale, tile_col, tile_row);
        free(tile_pixels);
    }
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

void philips_isyntax_flush_cache(openslide_t *osr) {
    // TODO(avirodov): seems to break image extraction. Reenable or have a better solution.
    return;
    // TODO(avirodov): This is not a perfect solution, as an LRU cache would be more efficient.
    LOG("@@@ philips_isyntax_flush_cache\n");
    isyntax_t *isyntax = osr->data;
    isyntax_image_t* wsi = &isyntax->images[isyntax->wsi_image_index];

    // This is the logic used by isyntax_do_first_load. I reuse it here to not flush out 
    // the first load tiles.
    i32 levels_in_top_chunk = (wsi->max_scale % 3) + 1;

    for (int level = wsi->max_scale - levels_in_top_chunk;  level >= 0; --level) {
        isyntax_level_t* current_level = &wsi->levels[level];
        LOG("@@@ unloadling level %d\n", current_level->scale);
        for (i32 tile_idx = 0; tile_idx < current_level->tile_count; ++tile_idx) {
            for (int channel_idx = 0; channel_idx < 3; ++channel_idx) {
                isyntax_tile_channel_t* channel = &current_level->tiles[tile_idx].color_channels[channel_idx];
                channel->neighbors_loaded = 0;
                if (channel->coeff_h) block_free(&isyntax->h_coeff_block_allocator, channel->coeff_h);
                if (channel->coeff_ll)  block_free(&isyntax->ll_coeff_block_allocator, channel->coeff_ll);
                channel->coeff_h = NULL;
                channel->coeff_ll = NULL;
            }
            current_level->tiles[tile_idx].has_ll = false;
            current_level->tiles[tile_idx].has_h = false;
            current_level->tiles[tile_idx].is_submitted_for_h_coeff_decompression = false;
            current_level->tiles[tile_idx].is_submitted_for_loading = false;
            current_level->tiles[tile_idx].is_loaded = false;
            current_level->tiles[tile_idx].force_reload = false;
        }
    }
}
#if 0
typedef struct isyntax_cache_entry_t {
    isyntax_tile_t* tile;
    int scale;
    int tile_col;
    int tile_row;
    struct isyntax_cache_entry_t* next;
    struct isyntax_cache_entry_t* prev;
} isyntax_cache_entry_t;

typedef struct isyntax_cache_list_t {
    struct isyntax_cache_entry_t* head;
    struct isyntax_cache_entry_t* tail;
} isyntax_cache_list_t;

static isyntax_cache_entry_t* isyntax_cache_list_remove()

};
#endif

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


static void isyntax_make_tile_lists_recursive(isyntax_t* isyntax, int scale, int tile_x, int tile_y,
                                                           bool is_idwt,
                                                           isyntax_tile_list_t* idwt_list,
                                                           isyntax_tile_list_t* coeff_list,
                                                           isyntax_tile_list_t* children_list,
                                                           isyntax_tile_list_t* cache_list) {
    isyntax_image_t* wsi = &isyntax->images[isyntax->wsi_image_index];
    isyntax_level_t* level = &wsi->levels[scale];
    if (tile_x < 0 || tile_x >= level->width_in_tiles || tile_y < 0 || tile_y >= level->height_in_tiles) {
        return;
    }

    isyntax_tile_t* tile = &level->tiles[level->width_in_tiles * tile_y + tile_x];
    if (tile->cache_marked || !tile->exists) {
        return;
    }

    // The tiles are currently either in the flat cache list, or uncached.
    tile_list_remove(cache_list, tile);
    tile->cache_marked = true;
    if (!is_idwt) {
        tile_list_insert_first(coeff_list, tile);
        // Note: no need to traverse neighbors of neighbors.
    } else {
        tile_list_insert_first(idwt_list, tile);

        // First process neighbors so that their parents (who require idwt) will be visited before they are visited
        // as neighbor of our parent. Consider:
        //    A
        //  B   C
        // D E F
        // If we start from E, and we visit B first, it will mark C as 'coeff_list' because all it needs is the
        // coefficients. However, node F needs the idwt of node C, so if we visit F first, it will land in the right
        // list on first visit, and we avoid relabeling.
        for (int y_offset = -1; y_offset <= 1; ++y_offset) {
            for (int x_offset = -1; x_offset <= 1; ++ x_offset) {
                // Invalid, missing, or self (already visited) tiles will be ignored. Not sure optimizing the call here
                // would speed anything up.
                // Note: neighbors don't require idwt, only coefficients.
                isyntax_make_tile_lists_recursive(isyntax, scale, tile_x + x_offset, tile_y + y_offset,
                                                  /*is_idwt=*/false, idwt_list, coeff_list, children_list, cache_list);
            }
        }
    }

    if (scale < isyntax->images[isyntax->wsi_image_index].max_scale) {
        // Now process parent tile. This one will require idwt.
        isyntax_make_tile_lists_recursive(isyntax, scale+1, tile_x / 2, tile_y / 2,
                                          /*is_idwt=*/true, idwt_list, coeff_list, children_list, cache_list);
    }

    // Now add children of idwt, unless they were already added in the other lists.
    // TODO(avirodov): if we store the idwt result (ll of next level) in the tile instead of the children, this
    //  would be unnecessary. But I'm not sure this is bad either.
    // TODO(avirodov): for now disabled, because neighbors end up in children list. Probably needs to be a separate
    //  pass.
    if (scale > 0 && false) {
        isyntax_level_t *next_level = &wsi->levels[scale - 1];
        isyntax_tile_t *child_top_left = next_level->tiles + (tile_y * 2) * next_level->width_in_tiles + (tile_x * 2);
        isyntax_tile_t *child_top_right = child_top_left + 1;
        isyntax_tile_t *child_bottom_left = child_top_left + next_level->width_in_tiles;
        isyntax_tile_t *child_bottom_right = child_bottom_left + 1;
        isyntax_tile_t* children[4] = {child_top_left, child_top_right, child_bottom_left, child_bottom_right};

        for (int i = 0; i < 4; ++i) {
            if (!children[i]->cache_marked) {
                tile_list_remove(cache_list, children[i]);
                tile_list_insert_first(children_list, children[i]);
            }
        }
    }
}

void isyntax_openslide_load_tile_coefficients(isyntax_t* isyntax, isyntax_tile_t* tile) {
    isyntax_image_t* wsi = &isyntax->images[isyntax->wsi_image_index];

    if (!tile->exists) {
        return;
    }

    // Load LL codeblocks here only for top-level tiles. For other levels, the LL coefficients are computed from parent
    // tiles later on.
    if (!tile->has_ll && tile->dbg_tile_scale == wsi->max_scale) {
        isyntax_data_chunk_t* chunk = &wsi->data_chunks[tile->data_chunk_index];

        for (int color = 0; color < 3; ++color) {
            isyntax_codeblock_t* codeblock = &wsi->codeblocks[tile->codeblock_index + color * chunk->codeblock_count_per_color];
            ASSERT(codeblock->coefficient == 0); // LL coefficient codeblock for this tile.
            ASSERT(codeblock->color_component == color);
            ASSERT(codeblock->scale == tile->dbg_tile_scale);
            tile->color_channels[color].coeff_ll = (icoeff_t*)block_alloc(&isyntax->ll_coeff_block_allocator);
            // TODO(avirodov): fancy allocators, for multiple sequential blocks (aka chunk). Or let OS do the caching.
            u8* codeblock_data = malloc(codeblock->block_size);
            size_t bytes_read = file_handle_read_at_offset(codeblock_data, isyntax->file_handle,
                                                           codeblock->block_data_offset, codeblock->block_size);
            if (!(bytes_read > 0)) {
                console_print_error("Error: could not read iSyntax data at offset %lld (read size %lld)\n", offset0, read_size);
            }

            isyntax_hulsken_decompress(codeblock_data, codeblock->block_size,
                                       isyntax->block_width, isyntax->block_height,
                                       codeblock->coefficient, 1, tile->color_channels[color].coeff_ll);
            free(codeblock_data);
        }
        tile->has_ll = true;
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

        for (int color = 0; color < 3; ++color) {
            isyntax_codeblock_t* codeblock = &wsi->codeblocks[tile->codeblock_chunk_index + codeblock_index_in_chunk + color * chunk->codeblock_count_per_color];
            ASSERT(codeblock->coefficient == 1);
            ASSERT(codeblock->color_component == color);
            ASSERT(codeblock->scale == tile->dbg_tile_scale);

            tile->color_channels[color].coeff_h = (icoeff_t*)block_alloc(&isyntax->h_coeff_block_allocator);
            // TODO(avirodov): fancy allocators, for multiple sequential blocks (aka chunk). Or let OS do the caching.
            u8* codeblock_data = malloc(codeblock->block_size);
            size_t bytes_read = file_handle_read_at_offset(codeblock_data, isyntax->file_handle,
                                                           codeblock->block_data_offset, codeblock->block_size);
            if (!(bytes_read > 0)) {
                console_print_error("Error: could not read iSyntax data at offset %lld (read size %lld)\n", offset0, read_size);
            }

            isyntax_hulsken_decompress(codeblock_data, codeblock->block_size,
                                       isyntax->block_width, isyntax->block_height,
                                       codeblock->coefficient, 1, tile->color_channels[color].coeff_h);
            free(codeblock_data);
        }
        tile->has_h = true;
    }
}

uint32_t* isyntax_openslide_idwt(isyntax_t* isyntax, isyntax_tile_t* tile, bool return_rgb) {
    if (tile->dbg_tile_scale == 0) {
        ASSERT(return_rgb); // Shouldn't be asking for idwt at level 0 if we're not going to use the result for pixels.
        return isyntax_load_tile(isyntax, &isyntax->images[isyntax->wsi_image_index], tile->dbg_tile_scale, tile->dbg_tile_x, tile->dbg_tile_y);
    }

    if (return_rgb) {
        // TODO(avirodov): if we want rgb from tile where idwt was done already, this could be cheaper if we store
        //  the lls in the tile. Currently need to recompute idwt.
        return isyntax_load_tile(isyntax, &isyntax->images[isyntax->wsi_image_index], tile->dbg_tile_scale, tile->dbg_tile_x, tile->dbg_tile_y);
    }

    // If all children have ll coefficients and we don't need the rgb pixels, no need to do the idwt.
    // TODO(avirodov): refactor.
    ASSERT(!return_rgb && tile->dbg_tile_scale > 0);
    isyntax_image_t* wsi = &isyntax->images[isyntax->wsi_image_index];
    isyntax_level_t *next_level = &wsi->levels[tile->dbg_tile_scale - 1];
    isyntax_tile_t *child_top_left = next_level->tiles + (tile->dbg_tile_y * 2) * next_level->width_in_tiles + (tile->dbg_tile_x * 2);
    isyntax_tile_t *child_top_right = child_top_left + 1;
    isyntax_tile_t *child_bottom_left = child_top_left + next_level->width_in_tiles;
    isyntax_tile_t *child_bottom_right = child_bottom_left + 1;

    if (child_top_left->has_ll && child_top_right->has_ll && child_bottom_left->has_ll && child_bottom_right->has_ll) {
        return NULL;
    }

    // TODO(avirodov): pass the 'return_rgb' flag to isyntax_load_tile so that we don't compute rgb if we don't need it.
    u32* result = isyntax_load_tile(isyntax, &isyntax->images[isyntax->wsi_image_index], tile->dbg_tile_scale, tile->dbg_tile_x, tile->dbg_tile_y);
    free(result);
    return NULL;
}


#define ITERATE_TILE_LIST(_iter, _list) \
    isyntax_tile_t* _iter = _list.head; _iter; _iter = _iter->cache_next

void isyntax_make_tile_lists_add_parent_to_list(isyntax_t* isyntax, isyntax_tile_t* tile, isyntax_tile_list_t* idwt_list, isyntax_tile_list_t* cache_list) {
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

void isyntax_make_tile_lists_add_children_to_list(isyntax_t* isyntax, isyntax_tile_t* tile, isyntax_tile_list_t* children_list, isyntax_tile_list_t* cache_list) {
    isyntax_image_t* wsi = &isyntax->images[isyntax->wsi_image_index];
    if (tile->dbg_tile_scale > 0) {
        isyntax_level_t *next_level = &wsi->levels[tile->dbg_tile_scale - 1];
        isyntax_tile_t *child_top_left = next_level->tiles + (tile->dbg_tile_y * 2) * next_level->width_in_tiles + (tile->dbg_tile_x * 2);
        isyntax_tile_t *child_top_right = child_top_left + 1;
        isyntax_tile_t *child_bottom_left = child_top_left + next_level->width_in_tiles;
        isyntax_tile_t *child_bottom_right = child_bottom_left + 1;
        isyntax_tile_t* children[4] = {child_top_left, child_top_right, child_bottom_left, child_bottom_right};

        for (int i = 0; i < 4; ++i) {
            if (!children[i]->cache_marked) {
                tile_list_remove(cache_list, children[i]);
                tile_list_insert_first(children_list, children[i]);
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
    for (ITERATE_TILE_LIST(tile, (*idwt_list))) {
        isyntax_make_tile_lists_add_children_to_list(isyntax, tile, children_list, cache_list);
    }
}

// TODO(avirodov): better global location (configurable), locking.
isyntax_tile_list_t cache_list = {NULL, NULL, 0, "cache_list"};

static uint32_t* isyntax_openslide_load_tile(isyntax_t* isyntax, int scale, int tile_x, int tile_y) {
    isyntax_image_t* wsi = &isyntax->images[isyntax->wsi_image_index];
    printf("=== isyntax_openslide_load_tile scale=%d tile_x=%d tile_y=%d\n", scale, tile_x, tile_y);

    // Need 3 lists:
    // 1. idwt list - those tiles will have to perform an idwt for their children to get ll coeffs. Primary cache bump.
    // 2. coeff list - those tiles are nighbors and will need to have coefficients loaded. Secondary cache bump.
    // 3. children list - those tiles will have their ll coeffs loaded as a side effect. Tertiary cache bump.
    // Those lists must be disjoint, and sorted such that parents are closer to head than children.
    isyntax_tile_list_t idwt_list = {NULL, NULL, 0, "idwt_list"};
    isyntax_tile_list_t coeff_list = {NULL, NULL, 0, "coeff_list"};
    isyntax_tile_list_t children_list = {NULL, NULL, 0, "children_list"};

    // Lock.
    // Make a list of all dependent tiles (including the required one).
    // Mark all dependent tiles as "reserved" so that they are not evicted as we load them.
    // Unlock.
    {
        isyntax_level_t* level = &wsi->levels[scale];
        isyntax_tile_t *tile = &level->tiles[level->width_in_tiles * tile_y + tile_x];
        tile_list_remove(&cache_list, tile);
        tile->cache_marked = true;
        tile_list_insert_first(&idwt_list, tile);
    }
    isyntax_make_tile_lists_by_scale(isyntax, scale, &idwt_list, &coeff_list, &children_list, &cache_list);
    // isyntax_make_tile_lists_recursive(isyntax, scale, tile_x, tile_y, /*is_idwt=*/true,
    //                                  &idwt_list, &coeff_list, &children_list, &cache_list);

    // Unmark visit status and reserve all nodes (todo later).
    for (ITERATE_TILE_LIST(tile, idwt_list))     { tile->cache_marked = false; /*printf("@@@ idwt_list tile scale=%d x=%d y=%d\n", tile->dbg_tile_scale, tile->dbg_tile_x, tile->dbg_tile_y);*/ }
    for (ITERATE_TILE_LIST(tile, coeff_list))    { tile->cache_marked = false; /*printf("@@@ coeff_list tile scale=%d x=%d y=%d\n", tile->dbg_tile_scale, tile->dbg_tile_x, tile->dbg_tile_y);*/ }
    for (ITERATE_TILE_LIST(tile, children_list)) { tile->cache_marked = false; /*printf("@@@ children_list tile scale=%d x=%d y=%d\n", tile->dbg_tile_scale, tile->dbg_tile_x, tile->dbg_tile_y);*/ }

    // IO+decode: For all dependent tiles, read and decode coefficients where missing (hh, and ll for top tiles).
    // Assuming lists are sorted parents first (by recursive construction).
    // IDWT as needed, top to bottom. This should produce idwt for this tile as well, which should be last in idwt list.
    // YCoCb->RGB for this tile only.
    uint32_t* result = NULL;
    for (ITERATE_TILE_LIST(tile, coeff_list)) {
        isyntax_openslide_load_tile_coefficients(isyntax, tile);
    }
    for (ITERATE_TILE_LIST(tile, idwt_list)) {
        isyntax_openslide_load_tile_coefficients(isyntax, tile);
    }
    for (ITERATE_TILE_LIST(tile, idwt_list)) {
        if (tile == idwt_list.tail) {
            result = isyntax_openslide_idwt(isyntax, tile, /*return_rgb=*/true);
        } else {
            isyntax_openslide_idwt(isyntax, tile, /*return_rgb=*/false);
        }
    }

    // Lock.
    // Bump all the affected tiles in cache.
    // Unmark all dependent tiles as "referenced" so that they can be evicted.
    // Perform cache trim (possibly not every invocation).
    // Unlock.

    tile_list_insert_list_first(&cache_list, &children_list);
    tile_list_insert_list_first(&cache_list, &coeff_list);
    tile_list_insert_list_first(&cache_list, &idwt_list);

    // Cache trim. Since we have the result already, it is possible that tiles from this run will be trimmed here
    // if cache is small or work happened on other threads.
    const int target_cache_size = 2000; // TODO(avirodov): configurable.
    while (cache_list.count > target_cache_size) {
        isyntax_tile_t* tile = cache_list.tail;
        tile_list_remove(&cache_list, tile);
        for (int i = 0; i < 3; ++i) {
            if (tile->has_ll) {
                block_free(&isyntax->ll_coeff_block_allocator, tile->color_channels[i].coeff_ll);
                tile->color_channels[i].coeff_ll = NULL;
            }
            if (tile->has_h) {
                block_free(&isyntax->h_coeff_block_allocator, tile->color_channels[i].coeff_h);
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
    isyntax_t *isyntax = osr->data;
    g_autoptr(GMutexLocker) locker G_GNUC_UNUSED =
            g_mutex_locker_new(&isyntax->read_mutex);

    philips_isyntax_level* pi_level = (philips_isyntax_level*)osr_level;
    isyntax_image_t* wsi_image = &isyntax->images[isyntax->wsi_image_index];

    // LOG("level=%d tile_col=%ld tile_row=%ld", pi_level->level_idx, tile_col, tile_row);
    // tile size
    int64_t tw = isyntax->tile_width;
    int64_t th = isyntax->tile_height;

    // cache
    g_autoptr(_openslide_cache_entry) cache_entry = NULL;
    uint32_t *tiledata = _openslide_cache_get(osr->cache,
                                              pi_level, tile_col, tile_row,
                                              &cache_entry);
    if (!tiledata) {
        tiledata = isyntax_openslide_load_tile(isyntax, pi_level->isyntax_level->scale, tile_col, tile_row);
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

static bool philips_isyntax_open(
        openslide_t *osr,
        const char *filename,
        struct _openslide_tifflike *tl G_GNUC_UNUSED,
        struct _openslide_hash *quickhash1 G_GNUC_UNUSED,
        GError **err) {
    static bool threadmemory_initialized = false;
    if (!threadmemory_initialized) {
        get_system_info(/*verbose=*/true);
        init_thread_memory(0);
    }
    LOG("Opening file %s", filename);

    isyntax_t *data = malloc(sizeof(isyntax_t));
    memset(data, 0, sizeof(isyntax_t));
    osr->data = data;

    bool open_result = isyntax_open(data, filename);
    LOG_VAR("%d", (int)open_result);
    LOG_VAR("%d", data->image_count);

    LOG_VAR("%d", data->is_mpp_known);
    if (data->is_mpp_known) {
        LOG_VAR("%f", data->mpp_x);
        LOG_VAR("%f", data->mpp_y);
        add_float_property(osr, OPENSLIDE_PROPERTY_NAME_MPP_X, data->mpp_x);
        add_float_property(osr, OPENSLIDE_PROPERTY_NAME_MPP_Y, data->mpp_x);
        const float float_equals_tolerance = 1e-5;
        if (fabsf(data->mpp_x - data->mpp_y) < float_equals_tolerance) {
            // Compute objective power from microns-per-pixel, see e.g. table in "Scan Performance" here:
            // https://www.microscopesinternational.com/blog/20170928-whichobjective.aspx
            float objective_power = 10.0f / data->mpp_x;
            LOG_VAR("%f", objective_power);
            add_float_property(osr, OPENSLIDE_PROPERTY_NAME_OBJECTIVE_POWER, objective_power);
        }
    }

    // Find wsi image. Extracting other images not supported. Assuming only one wsi.
    int wsi_image_idx = data->wsi_image_index;
    LOG_VAR("%d", wsi_image_idx);
    g_assert(wsi_image_idx >= 0 && wsi_image_idx < data->image_count);
    isyntax_image_t* wsi_image = &data->images[wsi_image_idx];

    // Store openslide information about each level.
    isyntax_level_t* levels = wsi_image->levels;
    osr->levels = malloc(sizeof(philips_isyntax_level*) * wsi_image->level_count);
    osr->level_count = wsi_image->level_count;
    for (int i = 0; i < wsi_image->level_count; ++i) {
        philips_isyntax_level* level = malloc(sizeof(philips_isyntax_level));
        level->isyntax_level = &wsi_image->levels[i];
        level->base.downsample = levels[i].downsample_factor;
        level->base.w = levels[i].width_in_tiles * data->tile_width;
        level->base.h = levels[i].height_in_tiles * data->tile_height;
        level->base.tile_w = data->tile_width;
        level->base.tile_h = data->tile_height;
        osr->levels[i] = (struct _openslide_level*)level;
        level->grid = _openslide_grid_create_simple(
                osr,
                levels[i].width_in_tiles,
                levels[i].height_in_tiles,
                data->tile_width,
                data->tile_height,
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
    g_mutex_init(&data->read_mutex);
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
    isyntax_t *data = osr->data;
    for (int i = 0; i < osr->level_count; ++i) {
        philips_isyntax_level* level = (philips_isyntax_level*)osr->levels[i];
        _openslide_grid_destroy(level->grid);
        free(level);
    }
    free(osr->levels);
    isyntax_destroy(data);
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
