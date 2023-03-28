#  LD_LIBRARY_PATH=/home/user/openslide/build/src/.libs python3 cache_evict_test.py

import openslide
import numpy as np
import PIL
from PIL import Image

if __name__ == "__main__":
    """ OpenSlide has its own cache that is finite, and thus can be evicted. iSyntax streamer tries not to send tiles
        that were already sent. This leads to situation where the tile was evicted from OpenSlide, but is not sent 
        again by the streamer, resulting in a white tile. """

    slide_path = 'golden-image-data/testslide.isyntax'
    slide = openslide.open_slide(slide_path)
    level = 0
    print('slide level dimensions: ' + str(slide.level_dimensions[level]))

    read1 = slide.read_region((19200, 19200), level, (1024, 1024))
    slide.set_cache(openslide.OpenSlideCache(1024*1024*128))  # 128Mb
    print("------ Cache reset ------")
    read2 = slide.read_region((19200, 19200), level, (1024, 1024))

    read1 = np.array(read1)
    read2 = np.array(read2)
    print(f'{read1.shape=}')
    print(f'{read2.shape=}')

    both_reads = np.concatenate((read1, read2), axis=1)
    print(f'{both_reads.shape=}')

    both_reads = Image.fromarray(both_reads)
    both_reads.save('extracted-image-data/cache_evict_test.png')


