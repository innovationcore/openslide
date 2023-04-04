/*
 *  Copyright (C) 2022  Alexandr Virodov
 *  All rights reserved.
 *
 *  OpenSlide is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU Lesser General Public License as
 *  published by the Free Software Foundation, version 2.1.
 *
 *  OpenSlide is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with OpenSlide. If not, see
 *  <http://www.gnu.org/licenses/>.
*/

#define STB_IMAGE_IMPLEMENTATION
#define STB_SPRINTF_IMPLEMENTATION
#include "stb_image.h"

#include <config.h>
#include <glib.h>
#include <string.h>
#include <tiffio.h>
#include "isyntax_reader.h"

#define IS_DEBUG_ANNOTATE_TILE false
#if IS_DEBUG_ANNOTATE_TILE
#include "font8x8_basic.h" // From https://github.com/dhepper/font8x8/blob/8e279d2d864e79128e96188a6b9526cfa3fbfef9/font8x8_basic.h
#endif

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
    isyntax_cache_t cache;
    GMutex mutex;
    // int refcount;
} philips_isyntax_cache_t;

typedef struct philips_isyntax_t {
    isyntax_t* isyntax;
    philips_isyntax_cache_t* cache;
} philips_isyntax_t;

// Global cache, shared between all opened files (if enabled). Thread-safe initialization in open().
philips_isyntax_cache_t* philips_isyntax_global_cache_ptr = NULL;

static void draw_horiz_line(uint32_t* tile_pixels, i32 tile_width, i32 y, i32 start, i32 end, uint32_t color) {
    for (int x = start; x < end; ++x) {
        tile_pixels[y*tile_width + x] = color;
    }
}

static void draw_vert_line(uint32_t* tile_pixels, i32 tile_width, i32 x, i32 start, i32 end, uint32_t color) {
    for (int y = start; y < end; ++y) {
        tile_pixels[y*tile_width + x] = color;
    }
}

static void draw_text(uint32_t* tile_pixels, i32 tile_width, i32 x_pos, i32 y_pos, uint32_t color, const char* text) {
#if IS_DEBUG_ANNOTATE_TILE
    const int font_size = 8;
    for (const char* ch = text; *ch != 0; ++ch) {
        for (int y = 0; y < font_size; ++y) {
            uint8_t bit_line = font8x8_basic[*((u8*)ch)][y];
            for (int x = 0; x < font_size; ++x) {
                if (bit_line & (1u << x)) {
                    tile_pixels[(y + y_pos) * tile_width + x + x_pos] = color;
                }
            }
        }
        x_pos += font_size;
    }
#endif
}

static void annotate_tile(uint32_t* tile_pixels, i32 scale, i32 tile_col, i32 tile_row, i32 tile_width, i32 tile_height) {
    if (IS_DEBUG_ANNOTATE_TILE) {
        // OpenCV in C is hard... the core_c.h includes types_c.h which includes cvdef.h which is c++.
        // But we don't need much. Axis-aligned lines, and some simple text.
        int pad = 1;
        uint32_t color = 0xff0000ff; // ARGB
        draw_horiz_line(tile_pixels, tile_width, /*y=*/pad, /*start=*/pad, /*end=*/tile_width - pad, color);
        draw_horiz_line(tile_pixels, tile_width, /*y=*/tile_height - pad, /*start=*/pad, /*end=*/tile_width - pad, color);

        draw_vert_line(tile_pixels, tile_width, /*x=*/pad, /*start=*/pad, /*end=*/tile_height - pad, color);
        draw_vert_line(tile_pixels, tile_width, /*x=*/tile_width - pad, /*start=*/pad, /*end=*/tile_height - pad, color);

        char buf[128];
        sprintf(buf, "x=%d,y=%d,s=%d", tile_row, tile_col, scale);
        draw_text(tile_pixels, tile_width, 10, 10, color, buf);
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
    LOG_VAR("%ld", num_read);
    LOG_VAR("%s", buf);

    // TODO(avirodov): probably a more robust XML parsing is needed.
    if (strstr(buf, "<DataObject ObjectType=\"DPUfsImport\">") != NULL) {
        LOG("got isyntax.");
        return true;
    }

    LOG("not isyntax.");
    return false;
}

static bool philips_isyntax_read_tile(
        openslide_t *osr,
        cairo_t *cr,
        struct _openslide_level *osr_level,
        int64_t tile_col, int64_t tile_row,
        void *arg G_GNUC_UNUSED,
        GError **err G_GNUC_UNUSED) {
    philips_isyntax_t *data = osr->data;
    isyntax_t* isyntax = data->isyntax;

    philips_isyntax_level* pi_level = (philips_isyntax_level*)osr_level;

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
        tiledata = isyntax_read_tile_bgra(&data->cache->cache, isyntax, pi_level->isyntax_level->scale, tile_col, tile_row);
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
    // https://lists.cairographics.org/archives/cairo/2012-June/023206.html
    // Those are the operators that are observed to work:
    //    w CAIRO_OPERATOR_SATURATE (current_cairo_operator, aka default OpenSlide)
    //    w CAIRO_OPERATOR_OVER ("This operator is cairo's default operator."),
    //    w CAIRO_OPERATOR_DEST_OVER,
    // SATURATE takes ~12sec to read a dummy slide (forcing all tiles to not exist), OVER & DEST_OVER take 3.5 sec
    // for same setup. Selecting OVER as the Cairo's default operator, the three outputs are identical.
    cairo_operator_t current_cairo_operator = cairo_get_operator(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
    cairo_paint(cr);
    cairo_set_operator(cr, current_cairo_operator);
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
    // Do not allow multithreading in opening.
    // https://docs.gtk.org/glib/method.Mutex.init.html:
    //   "It is not necessary to initialize a mutex that has been statically allocated."
    static GMutex static_open_mutex;
    g_mutex_lock(&static_open_mutex);
    static bool threadmemory_initialized = false;
    if (!threadmemory_initialized) {
        get_system_info(/*verbose=*/true);
        init_thread_memory(0);
        threadmemory_initialized = true;
    }
    g_mutex_unlock(&static_open_mutex);
    LOG("Opening file %s", filename);

    philips_isyntax_t* data = malloc(sizeof(philips_isyntax_t));
    data->isyntax = malloc(sizeof(isyntax_t));
    memset(data->isyntax, 0, sizeof(isyntax_t));

    osr->data = data;
    bool open_result = isyntax_open(data->isyntax, filename, /*init_allocators=*/false);
    LOG_VAR("%d", (int)open_result);
    LOG_VAR("%d", data->isyntax->image_count);
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
    {
        u64 memory_count = 0;
        isyntax_image_t* wsi = &data->isyntax->images[data->isyntax->wsi_image_index];
        for (int level_idx = 0; level_idx < wsi->level_count; ++level_idx) {
            memory_count += wsi->levels[level_idx].tile_count * sizeof(isyntax_tile_t);
        }
        memory_count += wsi->codeblock_count * sizeof(isyntax_codeblock_t);
        memory_count += wsi->data_chunk_count * sizeof(isyntax_data_chunk_t);
        printf("philips_isyntax_open is_global_cache=%d cache_size=%d sizeof_structs=%'lld\n", (int)is_global_cache, cache_size, memory_count);
    }
    if (is_global_cache) {
        g_mutex_lock(&static_open_mutex);
        if (philips_isyntax_global_cache_ptr == NULL) {
            // Note: this requires that all opened files have the same block size. If that is not true, we
            // will need to have allocator per size. Alternatively, implement allocator freeing after
            // all tiles have been freed, and track isyntax_t per tile so we can access allocator.
            philips_isyntax_global_cache_ptr = isyntax_make_cache("global_cache_list", cache_size,
                                                                  data->isyntax->block_width,
                                                                  data->isyntax->block_height);
        }
        data->cache = philips_isyntax_global_cache_ptr;
        g_mutex_unlock(&static_open_mutex);
    } else {
        data->cache = isyntax_make_cache("cache_list", cache_size,
                                         data->isyntax->block_width, data->isyntax->block_height);
    }
    ASSERT(data->isyntax->block_width == data->cache->cache.allocator_block_width);
    ASSERT(data->isyntax->block_height == data->cache->cache.allocator_block_height);

    LOG_VAR("%d", data->isyntax->is_mpp_known);
    if (data->isyntax->is_mpp_known) {
        LOG_VAR("%f", data->isyntax->mpp_x);
        LOG_VAR("%f", data->isyntax->mpp_y);
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
        while (data->cache->cache.cache_list.tail) {
            tile_list_remove(&data->cache->cache.cache_list, data->cache->cache.cache_list.tail);
        }

    } else {
        // Not shared (for now sharing is either global or none).
        if (data->cache->cache.ll_coeff_block_allocator.is_valid) {
            block_allocator_destroy(&data->cache->cache.ll_coeff_block_allocator);
        }
        if (data->cache->cache.h_coeff_block_allocator.is_valid) {
            block_allocator_destroy(&data->cache->cache.h_coeff_block_allocator);
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
