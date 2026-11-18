extern void __morgana_build(const char*, const char*, const char*, const char*);
extern void __morgana_upload(const char*, const char*, const char*, const char*, const char*);

int main() {
    __morgana_build(
        "./morgana-env/main.morg",
        "morgana-env/target",
        "avr_atmega328p",
        "morgana-env/Sake"
    );

    __morgana_upload(
        "morgana-env/target",
        "morgana-env/Sake",
        "avr_atmega328p",
        "",
        ""
    );
}
