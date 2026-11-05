
#include <stdio.h>
typedef char* cstring;
extern void __morgana_build(cstring mpath, cstring target, cstring ao, cstring libs, cstring sake, char useram, char enable_then);

int main() {
    __morgana_build(
        "./morgana-env/main.morg",
        "./morgana-env/target",
        "avr_atmega328p",
        "",
        "/home/dev_lucasfs/Documents/morgana/morgana-env/Sake",
        1,
        0
    );
    return 0;
}
