#pragma once

#include <cstddef>
#define EVA_VALUE_TAG_FIELDS \
    X(eva_string, "string") \
    X(eva_number, "number") \
    X(eva_bool, "bool") \
    X(eva_map, "map") \
    X(eva_list, "list") \
    X(eva_nil, "nil")

#define X(tag, ...) tag,
typedef enum { EVA_VALUE_TAG_FIELDS } EvaValueTag;
#undef X

typedef struct EvaParser {
    std::size_t status;
    void *parser;
} EvaParser;

typedef struct EvaValue {
    EvaValueTag tag;
    union {
        char *string;
        double number;
        int boolean;
    } data;
} EvaValue;

extern "C" {
    int set(char* f);
    int setoutput(char* f);
    int prepare(void);

    EvaParser *eva_make_parser(const char *path);
    void eva_dump_pointer(EvaValue value);
    EvaValue eva_get_value_from_namespace(EvaParser *parser, const char *ns, const char *name);
    char eva_check_exist_field_in_namespace(EvaParser *parser, const char *ns, const char *name);
    int eva_get_list_length(EvaValue list);
    EvaValue eva_get_list_field(EvaValue list, int index);
    int eva_get_map_length(EvaValue map);
    EvaValue eva_get_map_field(EvaValue map, const char *key);
    EvaValue eva_get_all_keys_from_map(EvaValue map);
    bool eva_check_exist_field_in_map(EvaValue map, const char *field);
}
