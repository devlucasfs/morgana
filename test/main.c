extern void __morgana_build(const char*, const char*, const char*);

int main() {
    __morgana_build("./morgana-env/main.morg", "morgana-env/target", "x86_64-gnu");
}
