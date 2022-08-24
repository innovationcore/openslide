#include <config.h>
#include <glib.h>
#include <string.h>
#include <tiffio.h>
#include "isyntax.h"
#include "font8x8_basic.h" // From https://github.com/dhepper/font8x8/blob/8e279d2d864e79128e96188a6b9526cfa3fbfef9/font8x8_basic.h

// This header "poisons" some functions, so must be included after system headers that use the poisoned functions (eg fclose in wchar.h).
#include "openslide-private.h"

#define LOG(msg, ...) printf("%s: " msg "\n", __FUNCTION__, ##__VA_ARGS__)
#define LOG_VAR(fmt, var) printf("%s: %s=" fmt "\n", __FUNCTION__, #var, var)

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

    if (!wsi_image->first_load_complete) {
        isyntax_do_first_load(osr, isyntax, wsi_image);
    }

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
        LOG("### isyntax_load_tile(x=%ld, y=%ld scale=%d)", tile_col, tile_row, pi_level->isyntax_level->scale);
        isyntax_level_t* stream_level = pi_level->isyntax_level;
        i32 px_offset_x = 0;
        i32 px_offset_y = 0;
        float um_offset_x = px_offset_x * stream_level->um_per_pixel_x;
        float um_offset_y = px_offset_y * stream_level->um_per_pixel_y;
        bounds2f camera_bounds = {
                .left = tile_col * stream_level->x_tile_side_in_um + um_offset_x,
                .right = (tile_col + 1) * stream_level->x_tile_side_in_um + um_offset_x,
                .top = tile_row * stream_level->y_tile_side_in_um + um_offset_y,
                .bottom = (tile_row+1) * stream_level->y_tile_side_in_um + um_offset_y,
        };
        tile_streamer_t tile_streamer = {
                .origin_offset = {
                        //-pi_level->origin_offset.x,
                        //-pi_level->origin_offset.y
                        0, 0
                },
                .camera_center = {
                        (camera_bounds.left + camera_bounds.right) / 2.0f,
                        (camera_bounds.top + camera_bounds.bottom) / 2.0f
                },
                .camera_bounds = camera_bounds,
                .is_cropped = false,
                .zoom_level = pi_level->isyntax_level->scale,
                .isyntax = isyntax,
                .userdata = osr,
        };
        isyntax_stream_image_tiles(&tile_streamer);
        // TODO(avirodov): assuming the streamer is hacked for immediate callback
        //   execution. Otherwise need to wait here.
        tiledata = _openslide_cache_get(osr->cache,
                                        pi_level, tile_col, tile_row,
                                        &cache_entry);
        // If there is no tiledata, it is because the tile doesn't exist in the .isyntax file (empty tiles are not
        // stored). Fill with background.
        // TODO(avirodov): it may be possible the cache failed to fill for other reasons. Would be nice to know
        //  specifically that this is due to tile->exists == false.
        if (tiledata == NULL) {
            LOG("missing tile (x=%ld, y=%ld), filling with background.", tile_col, tile_row);
            tiledata = malloc(tw * th * 4);
            memset(tiledata, 255, tw * th * 4);
            annotate_tile(tiledata, pi_level->isyntax_level->scale, tile_col, tile_row, tw, th);

            _openslide_cache_put(osr->cache, pi_level, tile_col, tile_row,
                                 tiledata, tw * th * 4,
                                 &cache_entry);
        }
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
