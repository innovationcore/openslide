set -e -x
WRITE_PNG=../../../build/tools/openslide-write-png

# NOTE: the file naming convention corresponds to coordinates that have the pixels of interest in the .tif extracted using the SDK. There are various offsets (765px fixed, +/-1px per level due to different roundings), so
# the subimages are extracted with the correct coords in the .isyntax, and named with the corresponding coords in .tiff.

compute_diff() {
  composite golden-image-data/$1 extracted-image-data/$1 -compose difference extracted-image-data/diff_$1
  convert extracted-image-data/diff_$1 -auto-level extracted-image-data/diffnorm_$1
  echo extracted-image-data/diff_$1 >> difflog.txt
  python3 eval_diff_image.py extracted-image-data/diff_$1 >> difflog.txt
}

write_png_whole() {
  $WRITE_PNG golden-image-data/testslide.isyntax 0 0 $1 $2 $3 extracted-image-data/testslide_0_0_37376_73216_$1.png
  compute_diff testslide_0_0_37376_73216_$1.png
}

write_png_part1() {
  $WRITE_PNG golden-image-data/testslide.isyntax $1 $2 $3 $4 $5 extracted-image-data/testslide_19200_19200_21248_21248_$3.png
  # compare -metric RMSE -subimage-search golden-image-data/testslide_19200_19200_21248_21248_$3.png extracted-image-data/testslide_19200_19200_21248_21248_$3.png extracted-image-data/diff_testslide_19200_19200_21248_21248_$3.png
  compute_diff testslide_19200_19200_21248_21248_$3.png
  convert extracted-image-data/testslide_19200_19200_21248_21248_$3.png -resize 2048x2048 extracted-image-data/samescale_testslide_19200_19200_21248_21248_$3.png
}

write_png_part1_tiff() {
  $WRITE_PNG golden-image-data/testslide.tiff $1 $2 $3 $4 $5 extracted-image-data/tiff_testslide_19200_19200_21248_21248_$3.png
}

write_png_corner_enhanced() {
  $WRITE_PNG golden-image-data/testslide.isyntax $1 $2 $3 $4 $5 extracted-image-data/testslide_0_0_4096_4096_$3.png
  convert -brightness-contrast -70x80 extracted-image-data/testslide_0_0_4096_4096_$3.png extracted-image-data/enhanced_testslide_0_0_4096_4096_$3.png
  compute_diff testslide_0_0_4096_4096_$3.png
}

rm -f difflog.txt

# Note: current implementation has a 765px padding for unknown reason.
write_png_corner_enhanced 765 765 0 4096 4096
write_png_corner_enhanced 765 765 1 2048 2048
write_png_corner_enhanced 765 765 2 1024 1024
write_png_corner_enhanced 765 765 3 512 512
write_png_corner_enhanced 765 765 4 256 256
write_png_corner_enhanced 765 765 5 128 128
write_png_corner_enhanced 765 765 6 64 64
write_png_corner_enhanced 765 765 7 32 32

write_png_whole 7 292 572
write_png_whole 6 584	1144
write_png_whole 5 1168	2288
write_png_whole 4 2336	4576
write_png_whole 3 4672	9152

write_png_part1 19965 19965 0 2048 2048
write_png_part1 19965 19965 1 1024 1024
write_png_part1 19965 19965 2 512 512
write_png_part1 19965 19965 3 256 256
write_png_part1 19965 19965 4 128 128
write_png_part1 19965 19965 5 64 64
write_png_part1 19965 19965 6 32 32
write_png_part1 19965 19965 7 16 16




# In golden, a point is at 112,893
# if extracting at 19200,19200, same point is at 877, 1658, so offset of 765 in both directions.
# TODO(avirodov): why not 19200?
#write_png_part1 19965 19965 0 2048 2048

# In golden, a point is at 55,446
# if extracting at 15000,15000, same point is at 2537, 2928, so offset of 2482 in both directions.
# write_png_part1 9982 9982 1 4096 4096


# TODO(avirodov): why this generates empty slide for tiff?
## later: write_png_part1_tiff 19194 19194 2 512 512
#write_png_part1_tiff 19200 19200 0 2048 2048
# TODO(avirodov): why 2px offset? Or should it be 19196? No, not 19196 - has an offset.
#write_png_part1_tiff 19198 19198 1 1024 1024
#write_png_part1_tiff 19192 19192 2 512 512
#write_png_part1_tiff 19184 19184 3 256 256
# TODO(avirodov): that's the right coords (if you go by y), but produces empty???
#write_png_part1_tiff 19180 19180 4 128 128
#write_png_part1_tiff 19160 19160 5 64 64
#write_png_part1_tiff 19120 19120 6 32 32
#write_png_part1_tiff 19060 19060 7 16 16


cat difflog.txt
echo num_zero_difference=`cat difflog.txt | grep 'min=0 max=0' | wc -l` num_cases=`cat difflog.txt | grep 'png' | wc -l`
