import openslide
import numpy as np
from enum import Enum
import sys

if __name__ == "__main__":
    """Reads each whole slide to test ML case & memory consumption."""
    # TODO(avirodov): args.
    iterations = int(sys.argv[1])
    level = int(sys.argv[2])
    slide_paths = sys.argv[3:]

    print(f'{slide_paths=}')
    print(f'{level=}')
    print(f'{iterations=}')

    slides = [openslide.open_slide(slide_path) for slide_path in slide_paths]
    tile_size = 224
    np.random.seed(1)
    random_offset = (0, 0)

    for _ in range(iterations):
        for slide in slides:
            level_dimensions = slide.level_dimensions[level]
            num_tiles_x = level_dimensions[0] // tile_size
            num_tiles_y = level_dimensions[1] // tile_size
            print(f'{slide.level_dimensions[0]=}')
            print(f'{slide.level_dimensions[level]=}')
            print(f'{num_tiles_x=} {num_tiles_y=}')
            sum_pixels = 0
            for j in range(num_tiles_y):
                print(f'reading row {level=} {j=} (out of {num_tiles_y=})')
                for i in range(num_tiles_x):
                    image = slide.read_region((i * tile_size, j * tile_size), level, (tile_size, tile_size))
                    sum_pixels += np.sum(np.array(image))
            print(f'{sum_pixels=}')

