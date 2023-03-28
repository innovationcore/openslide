#  LD_LIBRARY_PATH=/home/user/openslide/build/src/.libs python3 two_wsis.py

import openslide

if __name__ == "__main__":
    slide_one = openslide.open_slide('golden-image-data/deident.isyntax')
    slide_two = openslide.open_slide('golden-image-data/testslide.isyntax')
    level = 4

    image_one = slide_one.read_region((1024, 1024), level, (1024, 1024))
    print(f'{image_one=}')
    image_two = slide_two.read_region((1024, 1024), level, (1024, 1024))
    print(f'{image_two=}')

    image_one.save('extracted-image-data/two_wsis_image_one.png')
    image_two.save('extracted-image-data/two_wsis_image_two.png')
