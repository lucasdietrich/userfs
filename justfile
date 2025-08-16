# Justfile for managing build tasks
# Use `just <task>` to run a specific task
# Use `just --summary` to see available tasks
# QEMU
qemu *args: build
  ./scripts/run-qemu.sh {{exe}} {{args}}
qemu-debug *args: build
  ./scripts/run-qemu.sh --debug {{exe}} {{args}}

disassemble: build
  scripts/disassemble.sh build/userfs
clang-format:
  ./scripts/do-clang-format.sh

builddir := "build"
exe := "build/userfs"
target := "rpi3"

# Setup, runs only if builddir or ninja file is missing
setup:
  if [ ! -f {{builddir}}/build.ninja ]; then \
    meson {{builddir}} --buildtype=debug \
        -Dswap=true \
        -Dswap_partno=4 \
        -Duserfs_partno=5 \
  fi

setup_sdd:
  if [ ! -f {{builddir}}/build.ninja ]; then \
    meson {{builddir}} --buildtype=debug \
        -Dswap=true \
        -Dswap_partno=4 \
        -Duserfs_partno=5 \
        -Dblock_device_type=disk \
        -Dblock_device_name=/dev/sdd; \
  fi

reconfigure:
    meson setup {{builddir}} --reconfigure

build: setup
	meson compile -C {{builddir}}

deploy: build
  scp {{exe}} {{target}}:~

replace: build
  scp {{exe}} {{target}}:/usr/bin/userfs


clean:
    rm -rf {{builddir}}
setup-build: setup build
