
typedef char* cstring;
extern void __morgana_build(cstring mpath, cstring target, cstring extensor, cstring libs, cstring out);

int main() {
    __morgana_build("./morgana-env/main.morg", "./morgana-env/target", "./extensor.lua", "arena", "./output");
    return 0;
}
