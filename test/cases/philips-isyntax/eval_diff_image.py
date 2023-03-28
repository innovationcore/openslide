import os
import sys

import numpy as np
import cv2


def main():
    image_file = sys.argv[1]
    print(f'{os.getcwd()}')
    diff_image = cv2.imread(image_file)
    print(f'min={np.min(diff_image)} max={np.max(diff_image)}')

if __name__ == "__main__":
    main()