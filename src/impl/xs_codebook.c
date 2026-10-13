#include "xs_codebook.h"

#include "../compat/xs_compress.h"
#include "../util/hash.h"
#include "../util/io_util.h"
#include "../util/json.h"
#include "../util/string_util.h"
#include "../util/xs_bits.h"
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

enum { XS_BOOK_MAX_WORDS = 300000 };
static bool xs_word_valid(const char *word) {
  if (!word || strlen(word) < 2 || strlen(word) > 15) {
    return false;
  }
  for (const char *letter = word; *letter; letter++) {
    if (*letter < 'A' || *letter > 'Z') {
      return false;
    }
  }
  return true;
}
static int xs_word_order(const void *left, const void *right) {
  const XSWord *first = *(XSWord *const *)left;
  const XSWord *second = *(XSWord *const *)right;
  if (first->length != second->length) {
    return first->length < second->length ? -1 : 1;
  }
  return strcmp(first->word, second->word);
}
static bool xs_codebook_index(XSCodebook *book) {
  book->ordered = malloc_or_die(sizeof(*book->ordered) * (size_t)book->count);
  for (int index = 0; index < book->count; index++) {
    book->ordered[index] = &book->words[index];
  }
  qsort(book->ordered, (size_t)book->count, sizeof(*book->ordered),
        xs_word_order);
  uint64_t code = 0;
  unsigned previous = 0;
  for (int index = 0; index < book->count; index++) {
    XSWord *word = book->ordered[index];
    code <<= word->length - previous;
    if (code >= (UINT64_C(1) << word->length)) {
      return false;
    }
    word->code = (uint32_t)code;
    if (!book->counts[word->length]) {
      book->first[word->length] = (uint32_t)code;
      book->offsets[word->length] = (uint32_t)index;
    }
    book->counts[word->length]++;
    code++;
    previous = word->length;
  }
  return book->count == 1 || code == (UINT64_C(1) << previous);
}
void xs_codebook_destroy(XSCodebook *book) {
  if (book) {
    free(book->words);
    free(book->ordered);
    free(book);
  }
}
XSCodebook *xs_codebook_parse(const char *text, ErrorStack *errors) {
  const JsonValue *json = json_parse(text, errors);
  if (!json) {
    return NULL;
  }
  const char *format = json_get_string(json, "format", errors);
  const char *language = json_get_string(json, "language", errors);
  const char *lexicon = json_get_string(json, "lexicon", errors);
  const char *id = json_get_string(json, "id", errors);
  double version = json_get_double(json, "version", errors);
  const JsonValue *entries = json_object_get(json, "words");
  int count = json_array_length(entries);
  bool valid =
      error_stack_is_empty(errors) &&
      strings_equal(format, "xs-word-codebook") &&
      strings_equal(language, "en") && version == 1 &&
      ((strings_equal(lexicon, "CSW24") && strings_equal(id, "en-CSW24-v1")) ||
       (strings_equal(lexicon, "NWL23") && strings_equal(id, "en-NWL23-v1"))) &&
      count > 0 && count <= XS_BOOK_MAX_WORDS;
  XSCodebook *book = calloc_or_die(1, sizeof(*book));
  if (valid) {
    book->count = count;
    book->words = calloc_or_die((size_t)count, sizeof(*book->words));
    const JsonValue *entry = json_array_get(entries, 0);
    for (int index = 0; valid && index < count;
         index++, entry = json_next(entry)) {
      const char *word = json_array_get_string(entry, 0);
      double length = 0;
      bool number = json_number(json_array_get(entry, 1), &length);
      valid = number && json_array_length(entry) == 2 && xs_word_valid(word) &&
              length > 0 && length <= 32 && length == (unsigned)length &&
              (!index || strcmp(book->words[index - 1].word, word) < 0);
      if (valid) {
        memcpy(book->words[index].word, word, strlen(word) + 1);
        book->words[index].length = (unsigned)length;
      }
    }
  }
  json_destroy(json);
  if (valid) {
    valid = xs_codebook_index(book);
  }
  if (!valid) {
    xs_codebook_destroy(book);
    xs_error(errors, "invalid canonical word codebook");
    return NULL;
  }
  return book;
}
XSCodebook *xs_codebook_load(const char *data_paths, unsigned id,
                             ErrorStack *errors) {
  static const char *const names[] = {"", "CSW24.xsw.json.gz",
                                      "NWL23.xsw.json.gz"};
  static const char *const hashes[] = {
      "", "b23a65ddc28ff8452c9f012fae7692a60afd9897948f320abff1b5827cee566f",
      "697ea8a8c81ec7ba3cbc36428ad897eafdfabd4f89f53d30b5eaeebcb1936453"};
  if (id < 1 || id > 2) {
    xs_error(errors, "unknown codebook revision");
    return NULL;
  }
  char *path = xs_asset_path(data_paths, names[id], errors);
  if (!path) {
    return NULL;
  }
  size_t length = 0;
  char *raw = xs_read_file(path, &length, errors);
  free(path);
  if (!raw) {
    return NULL;
  }
  char *hash = sha256_hash_bytes(raw, length);
  bool valid = strings_equal(hash, hashes[id]);
  free(hash);
  size_t expanded = 0;
  char *text = valid ? xs_gunzip(raw, length, &expanded) : NULL;
  free(raw);
  if (!text || strlen(text) != expanded) {
    free(text);
    xs_error(errors, "word codebook checksum or gzip mismatch");
    return NULL;
  }
  XSCodebook *book = xs_codebook_parse(text, errors);
  free(text);
  return book;
}
const XSWord *xs_codebook_find(const XSCodebook *book, const char *word) {
  if (!book) {
    return NULL;
  }
  int low = 0;
  int high = book->count;
  while (low < high) {
    int middle = low + ((high - low) / 2);
    int comparison = strcmp(word, book->words[middle].word);
    if (!comparison) {
      return &book->words[middle];
    }
    if (comparison < 0) {
      high = middle;
    } else {
      low = middle + 1;
    }
  }
  return NULL;
}
const char *xs_codebook_read(const XSCodebook *book, XSBuffer *bits) {
  uint32_t code = 0;
  for (unsigned length = 1; bits->valid && length <= 32; length++) {
    code = (code << 1) | xs_get(bits, 1);
    if (book->counts[length] && code >= book->first[length] &&
        code - book->first[length] < book->counts[length]) {
      return book->ordered[book->offsets[length] + code - book->first[length]]
          ->word;
    }
  }
  bits->valid = false;
  return "";
}

typedef struct XSWeight {
  char word[16];
  uint64_t count;
  int parent;
  int minimum;
} XSWeight;
static int xs_weight_alpha(const void *left, const void *right) {
  return strcmp(((const XSWeight *)left)->word,
                ((const XSWeight *)right)->word);
}
static bool xs_weight_less(const XSWeight *nodes, int first, int second) {
  return nodes[first].count < nodes[second].count ||
         (nodes[first].count == nodes[second].count &&
          nodes[first].minimum < nodes[second].minimum);
}
static void xs_heap_push(int *heap, int *size, const XSWeight *nodes,
                         int node) {
  int index = (*size)++;
  while (index > 0 && xs_weight_less(nodes, node, heap[(index - 1) / 2])) {
    heap[index] = heap[(index - 1) / 2];
    index = (index - 1) / 2;
  }
  heap[index] = node;
}
static int xs_heap_pop(int *heap, int *size, const XSWeight *nodes) {
  int result = heap[0];
  int last = heap[--(*size)];
  int index = 0;
  while (index * 2 + 1 < *size) {
    int child = (index * 2) + 1;
    if (child + 1 < *size &&
        xs_weight_less(nodes, heap[child + 1], heap[child])) {
      child++;
    }
    if (!xs_weight_less(nodes, heap[child], last)) {
      break;
    }
    heap[index] = heap[child];
    index = child;
  }
  if (*size) {
    heap[index] = last;
  }
  return result;
}
bool xs_codebook_build(const char *input, const char *output,
                       const char *lexicon, ErrorStack *errors) {
  if (!strings_equal(lexicon, "CSW24") && !strings_equal(lexicon, "NWL23")) {
    xs_error(errors, "codebook lexicon must be CSW24 or NWL23");
    return false;
  }
  size_t bytes = 0;
  char *raw = xs_read_file(input, &bytes, errors);
  if (!raw) {
    return false;
  }
  if (bytes >= 2 && (unsigned char)raw[0] == 31 &&
      (unsigned char)raw[1] == 139) {
    size_t expanded = 0;
    char *text = xs_gunzip(raw, bytes, &expanded);
    free(raw);
    raw = text;
    bytes = expanded;
  }
  if (!raw || strlen(raw) != bytes) {
    free(raw);
    xs_error(errors, "invalid word count input");
    return false;
  }
  char *hash = sha256_hash_bytes(raw, bytes);
  StringSplitter *lines = split_string_by_newline(raw, true);
  free(raw);
  int count = string_splitter_get_number_of_items(lines);
  bool valid = count > 0 && count <= XS_BOOK_MAX_WORDS;
  XSWeight *nodes =
      valid ? calloc_or_die((size_t)count * 2, sizeof(*nodes)) : NULL;
  uint64_t total = 0;
  for (int index = 0; valid && index < count; index++) {
    StringSplitter *fields =
        split_string(string_splitter_get_item(lines, index), '\t', false);
    const char *word = string_splitter_get_number_of_items(fields) == 2
                           ? string_splitter_get_item(fields, 0)
                           : NULL;
    const char *frequency = word ? string_splitter_get_item(fields, 1) : "";
    errno = 0;
    char *end = NULL;
    unsigned long long weight = strtoull(frequency, &end, 10);
    valid = xs_word_valid(word) && *frequency >= '0' && *frequency <= '9' &&
            !errno && end && !*end && weight > 0 &&
            weight <= UINT64_MAX - total;
    if (valid) {
      strcpy(nodes[index].word, word);
      nodes[index].count = weight;
      nodes[index].parent = -1;
      total += weight;
    }
    string_splitter_destroy(fields);
  }
  string_splitter_destroy(lines);
  int *heap = valid ? malloc_or_die(sizeof(*heap) * (size_t)count) : NULL;
  int heap_size = 0;
  if (valid) {
    qsort(nodes, (size_t)count, sizeof(*nodes), xs_weight_alpha);
    for (int index = 0; index < count; index++) {
      if (index && strcmp(nodes[index - 1].word, nodes[index].word) == 0) {
        valid = false;
      }
      nodes[index].minimum = index;
      xs_heap_push(heap, &heap_size, nodes, index);
    }
  }
  for (int index = count; valid && heap_size > 1; index++) {
    int first = xs_heap_pop(heap, &heap_size, nodes);
    int second = xs_heap_pop(heap, &heap_size, nodes);
    nodes[index].count = nodes[first].count + nodes[second].count;
    nodes[index].minimum = nodes[first].minimum < nodes[second].minimum
                               ? nodes[first].minimum
                               : nodes[second].minimum;
    nodes[index].parent = -1;
    nodes[first].parent = index;
    nodes[second].parent = index;
    xs_heap_push(heap, &heap_size, nodes, index);
  }
  StringBuilder *json = string_builder_create();
  if (valid) {
    string_builder_add_formatted_string(
        json,
        "{\"format\":\"xs-word-codebook\",\"version\":1,\"id\":\"en-%s-v1\","
        "\"language\":\"en\",\"lexicon\":\"%s\",\"source_counts_sha256\":\"%"
        "s\",\"word_occurrences\":%llu,\"words\":[",
        lexicon, lexicon, hash, (unsigned long long)total);
    for (int index = 0; index < count; index++) {
      int depth = 0;
      for (int parent = nodes[index].parent; parent >= 0;
           parent = nodes[parent].parent) {
        depth++;
      }
      if (!depth) {
        depth = 1;
      }
      if (depth > 32) {
        valid = false;
      }
      string_builder_add_formatted_string(
          json, "%s[\"%s\",%d]", index ? "," : "", nodes[index].word, depth);
    }
    string_builder_add_string(json, "]}\n");
  }
  if (valid) {
    size_t compressed = 0;
    char *gzip = xs_gzip(string_builder_peek(json), string_builder_length(json),
                         &compressed);
    valid = gzip && xs_write_file(output, gzip, compressed, errors);
    free(gzip);
  }
  if (!valid && error_stack_is_empty(errors)) {
    xs_error(errors, "invalid counts or excessive Huffman depth");
  }
  string_builder_destroy(json);
  free(hash);
  free(nodes);
  free(heap);
  return valid;
}
