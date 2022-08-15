import openslide
import numpy as np
from enum import Enum

if __name__ == "__main__":
    # TODO(avirodov): args.
    slide_path = 'golden-image-data/testslide.isyntax'
    slide = openslide.open_slide(slide_path)
    levels = [2]
    tile_size = 224
    num_tiles_to_sample = 10000
    random_offset = (0, 0)
    mode = ''

    np.random.seed(1)

    for i in range(num_tiles_to_sample):
        level = np.random.choice(levels)
        level_dimensions = slide.level_dimensions[level]
        num_tiles_x = level_dimensions[0] // tile_size
        num_tiles_y = level_dimensions[1] // tile_size
        tile_location_x = np.random.choice(num_tiles_x) * tile_size + random_offset[0]
        tile_location_y = np.random.choice(num_tiles_y) * tile_size + random_offset[1]
        print(f'{i=} reading {level=} {tile_location_x=} {tile_location_y=}')
        image = slide.read_region((tile_location_x, tile_location_y), level, (tile_size, tile_size))





