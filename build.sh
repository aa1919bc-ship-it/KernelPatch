          export TARGET_COMPILE=`pwd`/arm-gnu-toolchain-12.2.rel1-x86_64-aarch64-none-elf/bin/aarch64-none-elf-
          cd kernel

          export ANDROID=1
          make
          mv kpimg kpimg-android
          mv kpimg.elf kpimg.elf-android
          make clean

