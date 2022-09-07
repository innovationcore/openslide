import openslide
import numpy as np
from enum import Enum
import sys

if __name__ == "__main__":
    """Reads the whole slide to test ML case & memory consumption."""
    # TODO(avirodov): args.
    level = int(sys.argv[1])
    slide_path = sys.argv[2]
    iterations = int(sys.argv[3])

    print(f'{slide_path=}')
    print(f'{level=}')
    print(f'{iterations=}')

    slide = openslide.open_slide(slide_path)
    tile_size = 224
    random_offset = (0, 0)

    np.random.seed(1)


    level_dimensions = slide.level_dimensions[level]
    num_tiles_x = level_dimensions[0] // tile_size
    num_tiles_y = level_dimensions[1] // tile_size
    print(f'{slide.level_dimensions[0]=}')
    print(f'{slide.level_dimensions[level]=}')
    print(f'{num_tiles_x=} {num_tiles_y=}')
    for _ in range(iterations):
        sum_pixels = 0
        for j in range(num_tiles_y):
            for i in range(num_tiles_x):
                print(f'reading {level=} {i=} {j=} (out of {num_tiles_y=})')
                image = slide.read_region((i * tile_size, j * tile_size), level, (tile_size, tile_size))
                sum_pixels += np.sum(np.array(image))
        print(f'{sum_pixels=}')






