#  LD_LIBRARY_PATH=/home/user/openslide/build/src/.libs python3 isyntax_test.py

import openslide
import threading

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

    def read_a_region_for_test():
        print(f'reading region from thread={threading.get_ident()}')
        whole_slide_image = slide.read_region((0, 0), level, slide.level_dimensions[level])

    # Read from main thread.
    read_a_region_for_test()

    # Read from another thread.
    new_thread = threading.Thread(target=read_a_region_for_test)
    new_thread.start()
    new_thread.join()

    # Read from main thread again.
    read_a_region_for_test()
