cd src

g++ -std=c++17 -g -ggdb  -O0 -fPIC -fpermissive -fexceptions \
    -shared main.cpp \
    -o ../bin/libmorgana.so \
    -I. \
   -L../libs/ -leva-x86_64 -lruna-x86_64 \
    -llzma -lm \
    $(pkg-config --static --libs libcurl) \
    -Wl,-rpath,'$ORIGIN'

cd ..

gcc -g -ggdb tests.c -L./bin -lmorgana -o ./bin/test.elf -Wl,-rpath,'$ORIGIN'
./bin/test.elf

cd morgana-env

# ../bin/morgana build -m main.morg -of native-bin -v
