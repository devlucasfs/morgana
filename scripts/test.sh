perl build.pl
gcc test/main.c -Lbin/ -lmorgana -o build/test.elf

LD_LIBRARY_PATH=bin:$LD_LIBRARY_PATH ./build/test.elf
