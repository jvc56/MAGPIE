#include "../src/ent/alias_method.h"
#include "../src/ent/klv.h"
#include "../src/ent/letter_distribution.h"
#include "../src/ent/players_data.h"
#include "../src/ent/rack.h"
#include "../src/ent/xoshiro.h"
#include "../src/impl/config.h"
#include "test_util.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct AliasMethodTestDistEntry {
  const char *rack_str;
  int count;
  int sampled_count;
  uint32_t klv_index;
} AliasMethodTestDistEntry;

void test_alias_method_dist(const Config *config, AliasMethod *am,
                            AliasMethodTestDistEntry *test_entries) {
  const LetterDistribution *ld = config_get_ld(config);
  const KLV *klv = players_data_get_klv(config_get_players_data(config), 0);
  XoshiroPRNG *prng = prng_create(0);
  const int ld_size = ld_get_size(ld);

  alias_method_reset(am);

  Rack rack;
  rack_set_dist_size_and_reset(&rack, ld_size);

  int num_entries = 0;
  int total_count = 0;
  while (test_entries[num_entries].rack_str != NULL) {
    AliasMethodTestDistEntry *entry = &test_entries[num_entries];
    entry->sampled_count = 0;
    rack_set_to_string(ld, &rack, entry->rack_str);
    entry->klv_index = klv_get_word_index(klv, &rack);
    alias_method_add_rack(am, &rack, entry->count);
    total_count += entry->count;
    num_entries++;
  }

  assert(alias_method_generate_tables(am));

  const int total_samples = 10000;

  for (int i = 0; i < total_samples; i++) {
    assert(alias_method_sample(am, prng_get_random_number(prng, XOSHIRO_MAX),
                               &rack));
    uint32_t klv_index = klv_get_word_index(klv, &rack);
    for (int j = 0; j < num_entries; j++) {
      AliasMethodTestDistEntry *entry = &test_entries[j];
      if (entry->klv_index == klv_index) {
        entry->sampled_count++;
        break;
      }
    }
  }

  printf("Sample Results:\n");
  for (int i = 0; i < num_entries; i++) {
    const AliasMethodTestDistEntry *entry = &test_entries[i];
    printf("%s: %d, %0.5f, %0.5f\n", entry->rack_str, entry->sampled_count,
           (double)entry->sampled_count / (double)total_samples,
           (double)entry->count / (double)total_count);
  }

  prng_destroy(prng);
}

void test_alias_method(void) {
  Config *config =
      config_create_or_die("set -lex CSW21 -s1 equity -s2 equity -r1 all -r2 "
                           "all -numplays 1  -gp false -threads 1");
  AliasMethod *am = alias_method_create();

  assert(!alias_method_generate_tables(am));
  assert(!alias_method_sample(am, 0, NULL));

  test_alias_method_dist(config, am,
                         (AliasMethodTestDistEntry[]){
                             {"A", 1, 0, 0},
                             {NULL, 0, 0, 0},
                         });

  test_alias_method_dist(config, am,
                         (AliasMethodTestDistEntry[]){
                             {"A", 1, 0, 0},
                             {"B", 1, 0, 0},
                             {"C", 1, 0, 0},
                             {NULL, 0, 0, 0},
                         });

  test_alias_method_dist(config, am,
                         (AliasMethodTestDistEntry[]){
                             {"A", 3, 0, 0},
                             {"B", 3, 0, 0},
                             {"C", 3, 0, 0},
                             {NULL, 0, 0, 0},
                         });

  test_alias_method_dist(config, am,
                         (AliasMethodTestDistEntry[]){
                             {"A", 1, 0, 0},
                             {"B", 2, 0, 0},
                             {"C", 3, 0, 0},
                             {NULL, 0, 0, 0},
                         });

  test_alias_method_dist(config, am,
                         (AliasMethodTestDistEntry[]){
                             {"A", 0, 0, 0},
                             {"B", 1, 0, 0},
                             {"C", 1, 0, 0},
                             {NULL, 0, 0, 0},
                         });

  test_alias_method_dist(config, am,
                         (AliasMethodTestDistEntry[]){
                             {"A", 0, 0, 0},
                             {"B", 10, 0, 0},
                             {"C", 0, 0, 0},
                             {NULL, 0, 0, 0},
                         });

  test_alias_method_dist(config, am,
                         (AliasMethodTestDistEntry[]){
                             {"A", 10, 0, 0},
                             {"B", 1, 0, 0},
                             {"C", 1, 0, 0},
                             {"D", 1, 0, 0},
                             {"E", 1, 0, 0},
                             {"F", 1, 0, 0},
                             {"G", 1, 0, 0},
                             {"H", 1, 0, 0},
                             {"I", 1, 0, 0},
                             {"J", 1, 0, 0},
                             {"K", 1, 0, 0},
                             {NULL, 0, 0, 0},
                         });
  alias_method_destroy(am);

  config_destroy(config);
}
