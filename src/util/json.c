#include "json.h"

#include "../compat/cjson.h"
#include <stdlib.h>
#include <string.h>

// JsonValue is cJSON behind an opaque name so the rest of MAGPIE never sees
// cJSON's types or conventions.
struct JsonValue;

static cJSON *as_cjson(const JsonValue *value) {
  return (cJSON *)(void *)(uintptr_t)value;
}

static JsonValue *as_json(cJSON *value) { return (JsonValue *)(void *)value; }

JsonValue *json_parse(const char *text, ErrorStack *error_stack) {
  cJSON *parsed = cJSON_Parse(text);
  if (!parsed) {
    const char *position = cJSON_GetErrorPtr();
    error_stack_push(
        error_stack, ERROR_STATUS_JSON_PARSE_FAILED,
        get_formatted_string("could not parse JSON near: %.60s",
                             position ? position : "(start of input)"));
    return NULL;
  }
  return as_json(parsed);
}

void json_destroy(const JsonValue *value) {
  if (value) {
    cJSON_Delete(as_cjson(value));
  }
}

const JsonValue *json_object_get(const JsonValue *object, const char *key) {
  if (!object) {
    return NULL;
  }
  return as_json(cJSON_GetObjectItemCaseSensitive(as_cjson(object), key));
}

bool json_is_null(const JsonValue *value) {
  return !value || cJSON_IsNull(as_cjson(value));
}

bool json_is_array(const JsonValue *value) {
  return value && cJSON_IsArray(as_cjson(value));
}

int json_array_length(const JsonValue *array) {
  if (!json_is_array(array)) {
    return 0;
  }
  return cJSON_GetArraySize(as_cjson(array));
}

const JsonValue *json_array_get(const JsonValue *array, int index) {
  if (!json_is_array(array)) {
    return NULL;
  }
  return as_json(cJSON_GetArrayItem(as_cjson(array), index));
}

static void push_missing(ErrorStack *error_stack, const char *key) {
  error_stack_push(error_stack, ERROR_STATUS_JSON_FIELD_MISSING,
                   get_formatted_string("JSON field '%s' is missing", key));
}

static void push_wrong_type(ErrorStack *error_stack, const char *key,
                            const char *expected) {
  error_stack_push(
      error_stack, ERROR_STATUS_JSON_FIELD_WRONG_TYPE,
      get_formatted_string("JSON field '%s' is not %s", key, expected));
}

const char *json_get_string(const JsonValue *object, const char *key,
                            ErrorStack *error_stack) {
  const JsonValue *field = json_object_get(object, key);
  if (!field) {
    push_missing(error_stack, key);
    return "";
  }
  if (!cJSON_IsString(as_cjson(field))) {
    push_wrong_type(error_stack, key, "a string");
    return "";
  }
  return as_cjson(field)->valuestring;
}

int64_t json_get_int(const JsonValue *object, const char *key,
                     ErrorStack *error_stack) {
  const JsonValue *field = json_object_get(object, key);
  if (!field) {
    push_missing(error_stack, key);
    return 0;
  }
  if (!cJSON_IsNumber(as_cjson(field))) {
    push_wrong_type(error_stack, key, "a number");
    return 0;
  }
  return (int64_t)as_cjson(field)->valuedouble;
}

double json_get_double(const JsonValue *object, const char *key,
                       ErrorStack *error_stack) {
  const JsonValue *field = json_object_get(object, key);
  if (!field) {
    push_missing(error_stack, key);
    return 0.0;
  }
  if (!cJSON_IsNumber(as_cjson(field))) {
    push_wrong_type(error_stack, key, "a number");
    return 0.0;
  }
  return as_cjson(field)->valuedouble;
}

uint64_t json_get_uint64_string(const JsonValue *object, const char *key,
                                ErrorStack *error_stack) {
  const JsonValue *field = json_object_get(object, key);
  if (!field) {
    push_missing(error_stack, key);
    return 0;
  }
  if (!cJSON_IsString(as_cjson(field))) {
    push_wrong_type(error_stack, key, "a decimal string");
    return 0;
  }
  const char *text = as_cjson(field)->valuestring;
  char *end = NULL;
  const unsigned long long parsed = strtoull(text, &end, 10);
  if (end == text || (end && *end != '\0')) {
    error_stack_push(
        error_stack, ERROR_STATUS_JSON_FIELD_WRONG_TYPE,
        get_formatted_string("JSON field '%s' is not a decimal integer: %s",
                             key, text));
    return 0;
  }
  return (uint64_t)parsed;
}

bool json_get_bool_or(const JsonValue *object, const char *key, bool fallback) {
  const JsonValue *field = json_object_get(object, key);
  if (!field || !cJSON_IsBool(as_cjson(field))) {
    return fallback;
  }
  return cJSON_IsTrue(as_cjson(field));
}

int json_get_int_or(const JsonValue *object, const char *key, int fallback) {
  const JsonValue *field = json_object_get(object, key);
  if (!field || !cJSON_IsNumber(as_cjson(field))) {
    return fallback;
  }
  return (int)as_cjson(field)->valuedouble;
}

double json_get_double_or(const JsonValue *object, const char *key,
                          double fallback) {
  const JsonValue *field = json_object_get(object, key);
  if (!field || !cJSON_IsNumber(as_cjson(field))) {
    return fallback;
  }
  return as_cjson(field)->valuedouble;
}

const char *json_get_string_or_null(const JsonValue *object, const char *key) {
  const JsonValue *field = json_object_get(object, key);
  if (!field || !cJSON_IsString(as_cjson(field))) {
    return NULL;
  }
  return as_cjson(field)->valuestring;
}

// --- Serialization ---------------------------------------------------------

static void write_separator(StringBuilder *sb, bool *first) {
  if (*first) {
    *first = false;
  } else {
    string_builder_add_string(sb, ",");
  }
}

void json_write_quoted(StringBuilder *sb, const char *value) {
  string_builder_add_string(sb, "\"");
  for (const char *c = value; *c; c++) {
    switch (*c) {
    case '"':
      string_builder_add_string(sb, "\\\"");
      break;
    case '\\':
      string_builder_add_string(sb, "\\\\");
      break;
    case '\n':
      string_builder_add_string(sb, "\\n");
      break;
    case '\r':
      string_builder_add_string(sb, "\\r");
      break;
    case '\t':
      string_builder_add_string(sb, "\\t");
      break;
    default:
      if ((unsigned char)*c < 0x20) {
        string_builder_add_formatted_string(sb, "\\u%04x", (unsigned char)*c);
      } else {
        string_builder_add_formatted_string(sb, "%c", *c);
      }
      break;
    }
  }
  string_builder_add_string(sb, "\"");
}

void json_write_object_start(StringBuilder *sb) {
  string_builder_add_string(sb, "{");
}

void json_write_object_end(StringBuilder *sb) {
  string_builder_add_string(sb, "}");
}

void json_write_raw_key(StringBuilder *sb, const char *key, bool *first) {
  write_separator(sb, first);
  json_write_quoted(sb, key);
  string_builder_add_string(sb, ":");
}

void json_write_array_start(StringBuilder *sb, const char *key, bool *first) {
  json_write_raw_key(sb, key, first);
  string_builder_add_string(sb, "[");
}

void json_write_array_end(StringBuilder *sb) {
  string_builder_add_string(sb, "]");
}

void json_write_int_field(StringBuilder *sb, const char *key, int64_t value,
                          bool *first) {
  json_write_raw_key(sb, key, first);
  string_builder_add_formatted_string(sb, "%lld", (long long)value);
}

void json_write_double_field(StringBuilder *sb, const char *key, double value,
                             bool *first) {
  json_write_raw_key(sb, key, first);
  // Fixed notation under the C locale: %g would emit exponents the server's
  // JSON reader accepts but which read badly, and a comma decimal separator
  // under a different locale would be invalid JSON outright.
  string_builder_add_formatted_string(sb, "%.6f", value);
}

void json_write_string_field(StringBuilder *sb, const char *key,
                             const char *value, bool *first) {
  json_write_raw_key(sb, key, first);
  json_write_quoted(sb, value ? value : "");
}
