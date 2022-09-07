import openslide
import numpy as np
from enum import Enum
import sys

if __name__ == "__main__":
    """Randomly samples each whole slide to test ML case & memory consumption."""
    # TODO(avirodov): args.
    iterations = int(sys.argv[1])
    samples = int(sys.argv[2])
    slide_paths = sys.argv[3:]

    levels = [0, 1, 2]
    print(f'{iterations=}')
    print(f'{samples=}')
    print(f'{slide_paths=}')
    print(f'{levels=}')

    slides = [openslide.open_slide(slide_path) for slide_path in slide_paths]
    tile_size = 224
    np.random.seed(1)
    random_offset = (0, 0)

    for _ in range(iterations):
        for slide in slides:
            for i in range(samples):
                level = np.random.choice(levels)
                level_dimensions = slide.level_dimensions[level]
                num_tiles_x = level_dimensions[0] // tile_size
                num_tiles_y = level_dimensions[1] // tile_size
                tile_location_x = np.random.choice(num_tiles_x) * tile_size + random_offset[0]
                tile_location_y = np.random.choice(num_tiles_y) * tile_size + random_offset[1]
                print(f'{i=} reading {level=} {tile_location_x=} {tile_location_y=}')
                image = slide.read_region((tile_location_x, tile_location_y), level, (tile_size, tile_size))
