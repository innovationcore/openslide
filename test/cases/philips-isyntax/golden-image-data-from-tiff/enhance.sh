set -e -x

for level in 7 6 5 4 3 2 1 0; do
  convert -brightness-contrast -70x80 testslide_0_0_4096_4096_$level.png enhanced_testslide_0_0_4096_4096_$level.png
done
