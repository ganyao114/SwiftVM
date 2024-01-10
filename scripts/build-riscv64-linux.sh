cd ../ || exit
mkdir build-riscv64-linux
cd build-riscv64-linux || exit
cmake ../ -G Ninja -DCMAKE_C_COMPILER=riscv64-unknown-elf-gcc -DCMAKE_CXX_COMPILER=riscv64-unknown-elf-g++ -DCMAKE_BUILD_TYPE=Release