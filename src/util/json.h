#ifndef JSON_H
#define JSON_H

// ErrorStack-aware wrapper over the vendored cJSON. Nothing outside this file
// includes cjson.h, so swapping the parser touches one translation unit.

#include <stdbool.h>
#include <stdint.h>

#include "io_util.h"
#include "string_util.h"

typedef struct JsonValue JsonValue;

// Parses `text`. Returns NULL and pushes onto the stack on malformed input.
JsonValue *json_parse(const char *text, ErrorStack *error_stack);
void json_destroy(JsonValue *value);

const JsonValue *json_object_get(const JsonValue *object, const char *key);
bool json_is_null(const JsonValue *value);
bool json_is_array(const JsonValue *value);
int json_array_length(const JsonValue *array);
const JsonValue *json_array_get(const JsonValue *array, int index);

// Each pushes onto the error stack when the key is absent or the wrong type,
// and returns a zero value. Callers check the stack once at the end rather
// than after every field.
const char *json_get_string(const JsonValue *object, const char *key,
                            ErrorStack *error_stack);
int64_t json_get_int(const JsonValue *object, const char *key,
                     ErrorStack *error_stack);
double json_get_double(const JsonValue *object, const char *key,
                       ErrorStack *error_stack);

// `seed` crosses the wire as a decimal string because it is a full uint64 and
// JSON numbers are doubles, which lose precision above 2^53.
uint64_t json_get_uint64_string(const JsonValue *object, const char *key,
                               ErrorStack *error_stack);

// Absent or null yields the fallback rather than an error, for optional fields.
bool json_get_bool_or(const JsonValue *object, const char *key, bool fallback);
int json_get_int_or(const JsonValue *object, const char *key, int fallback);
double json_get_double_or(const JsonValue *object, const char *key,
                          double fallback);
// Returns NULL when absent or null. The pointer belongs to the JsonValue.
const char *json_get_string_or_null(const JsonValue *object, const char *key);

// --- Serialization ---------------------------------------------------------
// Thin helpers over StringBuilder. They track nothing, so the caller is
// responsible for balancing braces; `first` guards comma placement.

void json_write_object_start(StringBuilder *sb);
void json_write_object_end(StringBuilder *sb);
void json_write_array_start(StringBuilder *sb, const char *key, bool *first);
void json_write_array_end(StringBuilder *sb);
void json_write_raw_key(StringBuilder *sb, const char *key, bool *first);
void json_write_int_field(StringBuilder *sb, const char *key, int64_t value,
                          bool *first);
void json_write_double_field(StringBuilder *sb, const char *key, double value,
                             bool *first);
void json_write_string_field(StringBuilder *sb, const char *key,
                             const char *value, bool *first);
// Escapes per RFC 8259 and appends a quoted string.
void json_write_quoted(StringBuilder *sb, const char *value);

#endif
