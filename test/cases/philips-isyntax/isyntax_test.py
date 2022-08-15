#  LD_LIBRARY_PATH=/home/user/openslide/build/src/.libs python3 isyntax_test.py

import openslide

if __name__ == "__main__":
    #slide to process
    slide_path = 'golden-image-data/deident.isyntax'
    #scale factor to process image
    SCALE_FACTOR = 32
    #open the slide
    slide = openslide.open_slide(slide_path)
    level = slide.get_best_level_for_downsample(SCALE_FACTOR)
    print('best level: ' + str(level))
    print('slide level dimensions: ' + str(slide.level_dimensions[level]))
    #fails on next function
    whole_slide_image = slide.read_region((0, 0), level, slide.level_dimensions[level])