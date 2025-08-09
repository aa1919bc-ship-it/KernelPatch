          export TARGET_COMPILE=`pwd`/arm-gnu-toolchain-12.2.rel1-x86_64-aarch64-none-elf/bin/aarch64-none-elf-
          cd kernel
asdf
          export ANDROID=1
          make -j 8
          mv kpimg kpimg-android
          mv kpimg.elf kpimg.elf-android
          make clean

