import numpy as np
import sys
import time

import os
if hasattr(os, 'add_dll_directory'):
    # Python >= 3.8 on Windows
    with os.add_dll_directory(r'C:\projects\software\openslide-win64-20171122\bin'):
        import openslide
else:
    import openslide

tile_size = 256
random_offset = (0, 0)


def sequential_read(levels, slides):
    iteration = 0
    for slide_idx, (slide, level) in enumerate(zip(slides, levels)):
        level_dimensions = slide.level_dimensions[level]
        num_tiles_x = level_dimensions[0] // tile_size
        num_tiles_y = level_dimensions[1] // tile_size
        print(f'{slide.level_dimensions[0]=}')
        print(f'{slide.level_dimensions[level]=}')
        print(f'{num_tiles_x=} {num_tiles_y=}')
        sum_pixels = 0
        for j in range(num_tiles_y):
            print(f'{iteration=} {slide_idx=} reading row {level=} {j=} (out of {num_tiles_y=})')
            for i in range(num_tiles_x):
                image = slide.read_region((i * tile_size, j * tile_size), level, (tile_size, tile_size))
                sum_pixels += np.sum(np.array(image))
        print(f'{sum_pixels=}')


def random_read(levels, slides, num_tiles):
    for slide_idx, (slide, level) in enumerate(zip(slides, levels)):
        level_dimensions = slide.level_dimensions[level]
        num_tiles_x = level_dimensions[0] // tile_size
        num_tiles_y = level_dimensions[1] // tile_size
        print(f'{slide.level_dimensions[0]=}')
        print(f'{slide.level_dimensions[level]=}')
        print(f'{num_tiles_x=} {num_tiles_y=} {num_tiles}')
        sum_pixels = 0
        for i in range(num_tiles):
            tile_location_x = np.random.choice(num_tiles_x) * tile_size + random_offset[0]
            tile_location_y = np.random.choice(num_tiles_y) * tile_size + random_offset[1]
            # print(f'{i=} reading {level=} {tile_location_x=} {tile_location_y=}')
            image = slide.read_region((tile_location_x, tile_location_y), level, (tile_size, tile_size))
            sum_pixels += np.sum(np.array(image))
        print(f'{sum_pixels=}')


def open_slides(slide_paths):
    return [openslide.open_slide(slide_path) for slide_path in slide_paths]


def profile_call(label, a_call):
    print(f'== Start profiling {label=}==')
    start_time = time.perf_counter()
    result = a_call()
    end_time = time.perf_counter()
    print(f'== End profiling {end_time-start_time=:0.3f} (s) ==')
    return result


def main():
    # TODO(avirodov): args.
    test = int(sys.argv[1])
    slide_paths = sys.argv[2:]
    print(f'{test=} {slide_paths[:10]=}')

    num_tiles_for_multi_tile_read = 100
    np.random.seed()

    print(f'{slide_paths=}')
    slides = profile_call('open', lambda: open_slides(slide_paths))
    intermediate_levels = [slide.level_count // 2 for slide in slides]
    last_levels = [0] * len(slides)
    print(f'{slides[0].level_dimensions=} {intermediate_levels[0]=} {last_levels[0]=}')
    if test == 0:
        # Opening only, do nothing.
        pass
    elif test == 1:
        # Full Sequential read of last level
        profile_call('sequential, last', lambda: sequential_read(last_levels, slides))
    elif test == 2:
        # Full Sequential read of intermediate level in pyramid
        profile_call('sequential, intermediate', lambda: sequential_read(intermediate_levels, slides))
    elif test == 3:
        # ** Full Sequential read of intermediate level not in pyramid -> is possible? Or rescale?
        pass
    elif test == 4:
        # Random single tile read of last level
        profile_call('random 1, last', lambda: random_read(last_levels, slides, 1))
    elif test == 5:
        # Random single tile read of intermediate level in pyramid
        profile_call('random 1, intermediate', lambda: random_read(intermediate_levels, slides, 1))
    elif test == 6:
        # ** Random single tile read of intermediate level not in pyramid -> is possible? Or rescale?
        pass
    elif test == 7:
        # Random 100 tile read of last level
        profile_call('random n, last', lambda: random_read(last_levels, slides, num_tiles_for_multi_tile_read))
    elif test == 8:
        # Random 100 tile read of intermediate level in pyramid
        profile_call('random n, intermediate', lambda: random_read(intermediate_levels, slides, num_tiles_for_multi_tile_read))
    elif test == 9:
        # ** Random 100 tile read of intermediate level not in pyramid -> is possible? Or rescale?
        pass


if __name__ == "__main__":
    main()