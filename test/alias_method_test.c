#include "alias_method_test.h"

#include "../src/def/letter_distribution_defs.h"
#include "../src/ent/alias_method.h"
#include "../src/ent/encoded_rack.h"
#include "../src/ent/klv.h"
#include "../src/ent/letter_distribution.h"
#include "../src/ent/players_data.h"
#include "../src/ent/rack.h"
#include "../src/ent/xoshiro.h"
#include "../src/impl/config.h"
#include "test_util.h"
#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

  const bool generated = alias_method_generate_tables(am);

  assert(generated);

  const int total_samples = 10000;

  for (int i = 0; i < total_samples; i++) {
    assert(alias_method_sample(am, prng, &rack));
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

// The same items added in opposite orders draw the same racks from
// identically seeded PRNGs.
static void test_alias_method_order_independent(const Config *config) {
  const LetterDistribution *ld = config_get_ld(config);
  const char *rack_strs[] = {"AE", "S", "?Q", "ER", "S", "DEIR", "Z"};
  const int counts[] = {3, 5, 1, 2, 4, 7, 2};
  const int num_items = (int)(sizeof(counts) / sizeof(counts[0]));
  AliasMethod *forward = alias_method_create();
  AliasMethod *backward = alias_method_create();
  Rack rack;
  Rack backward_rack;
  rack_set_dist_size_and_reset(&rack, ld_get_size(ld));
  rack_set_dist_size_and_reset(&backward_rack, ld_get_size(ld));
  for (int item_idx = 0; item_idx < num_items; item_idx++) {
    rack_set_to_string(ld, &rack, rack_strs[item_idx]);
    alias_method_add_rack(forward, &rack, counts[item_idx]);
    const int reverse_idx = num_items - 1 - item_idx;
    rack_set_to_string(ld, &rack, rack_strs[reverse_idx]);
    alias_method_add_rack(backward, &rack, counts[reverse_idx]);
  }
  const bool forward_generated = alias_method_generate_tables(forward);
  assert(forward_generated);
  const bool backward_generated = alias_method_generate_tables(backward);
  assert(backward_generated);
  XoshiroPRNG *forward_prng = prng_create(7);
  XoshiroPRNG *backward_prng = prng_create(7);
  for (int sample_idx = 0; sample_idx < 1000; sample_idx++) {
    assert(alias_method_sample(forward, forward_prng, &rack));
    assert(alias_method_sample(backward, backward_prng, &backward_rack));
    assert(racks_are_equal(&rack, &backward_rack));
  }
  prng_destroy(forward_prng);
  prng_destroy(backward_prng);
  alias_method_destroy(forward);
  alias_method_destroy(backward);
}

static void test_alias_method_zero_weight(const Config *config) {
  AliasMethod *am = alias_method_create();
  Rack rack;
  rack_set_dist_size_and_reset(&rack, ld_get_size(config_get_ld(config)));
  rack_set_to_string(config_get_ld(config), &rack, "AE");
  alias_method_add_weighted_rack(am, &rack, 10, 0);
  const bool zero_weight_generated = alias_method_generate_tables(am);
  assert(!zero_weight_generated);
  XoshiroPRNG *prng = prng_create(7);
  assert(!alias_method_sample(am, prng, &rack));
  alias_method_reset(am);
  alias_method_add_weighted_rack(am, &rack, 10, 0.5);
  const bool positive_weight_generated = alias_method_generate_tables(am);
  assert(positive_weight_generated);
  assert(alias_method_sample(am, prng, &rack));
  prng_destroy(prng);
  alias_method_destroy(am);
}

enum {
  NUM_EXCHANGE_SAMPLES = 20000,
  NUM_EXCHANGE_LETTERS = 26,
};

typedef struct AliasMethodTestExchange {
  const char *kept_str;
  const char *exchanged_str;
  double weight;
  int count;
  int sampled_count;
} AliasMethodTestExchange;

static void add_test_exchange(const LetterDistribution *ld, AliasMethod *am,
                              const AliasMethodTestExchange *exchange,
                              const bool weighted) {
  Rack kept;
  Rack exchanged;
  rack_set_to_string(ld, &kept, exchange->kept_str);
  rack_set_to_string(ld, &exchanged, exchange->exchanged_str);
  if (weighted) {
    alias_method_add_weighted_exchange(am, &kept, &exchanged, exchange->count,
                                       exchange->weight);
  } else {
    alias_method_add_exchange(am, &kept, &exchanged, exchange->count);
  }
}

// Exchanges added in opposite orders draw the same kept racks and exchanged
// tiles from identically seeded PRNGs: each kept rack with the tiles it was
// added with, as often as its count or weight says. Several of the exchanges
// differ only in the tiles thrown back, and one keeps nothing.
static void test_alias_method_exchange_order_independent(const Config *config,
                                                         const bool weighted) {
  const LetterDistribution *ld = config_get_ld(config);
  AliasMethodTestExchange exchanges[] = {
      {"ES", "QVV", 1.5, 3, 0},   {"ES", "QUV", 1.5, 3, 0},
      {"ES", "JQV", 1.5, 3, 0},   {"?", "UUVVWW", 4.0, 1, 0},
      {"", "IIIOUUU", 0.5, 2, 0}, {"ERS", "VV", 1.0, 5, 0},
      {"ES", "BQ", 1.5, 3, 0},    {"ERS", "QVV", 1.0, 5, 0},
  };
  const int num_exchanges = (int)(sizeof(exchanges) / sizeof(exchanges[0]));
  AliasMethod *forward = alias_method_create();
  AliasMethod *backward = alias_method_create();
  double total = 0;
  for (int exchange_idx = 0; exchange_idx < num_exchanges; exchange_idx++) {
    add_test_exchange(ld, forward, &exchanges[exchange_idx], weighted);
    add_test_exchange(ld, backward,
                      &exchanges[num_exchanges - 1 - exchange_idx], weighted);
    total += weighted ? exchanges[exchange_idx].weight
                      : (double)exchanges[exchange_idx].count;
  }
  assert(alias_method_has_exchanged_racks(forward));
  const bool forward_generated = alias_method_generate_tables(forward);
  assert(forward_generated);
  const bool backward_generated = alias_method_generate_tables(backward);
  assert(backward_generated);

  XoshiroPRNG *forward_prng = prng_create(7);
  XoshiroPRNG *backward_prng = prng_create(7);
  XoshiroPRNG *kept_only_prng = prng_create(7);
  Rack kept;
  Rack exchanged;
  Rack backward_kept;
  Rack backward_exchanged;
  Rack kept_only;
  Rack expected_kept;
  Rack expected_exchanged;
  rack_set_dist_size_and_reset(&kept, ld_get_size(ld));
  rack_set_dist_size_and_reset(&exchanged, ld_get_size(ld));
  rack_set_dist_size_and_reset(&backward_kept, ld_get_size(ld));
  rack_set_dist_size_and_reset(&backward_exchanged, ld_get_size(ld));
  rack_set_dist_size_and_reset(&kept_only, ld_get_size(ld));
  for (int sample_idx = 0; sample_idx < NUM_EXCHANGE_SAMPLES; sample_idx++) {
    assert(
        alias_method_sample_exchange(forward, forward_prng, &kept, &exchanged));
    assert(alias_method_sample_exchange(backward, backward_prng, &backward_kept,
                                        &backward_exchanged));
    assert(racks_are_equal(&kept, &backward_kept));
    assert(racks_are_equal(&exchanged, &backward_exchanged));
    // Sampling the kept rack alone takes the same item.
    assert(alias_method_sample(forward, kept_only_prng, &kept_only));
    assert(racks_are_equal(&kept, &kept_only));
    bool found = false;
    for (int exchange_idx = 0; exchange_idx < num_exchanges; exchange_idx++) {
      rack_set_to_string(ld, &expected_kept, exchanges[exchange_idx].kept_str);
      rack_set_to_string(ld, &expected_exchanged,
                         exchanges[exchange_idx].exchanged_str);
      if (racks_are_equal(&kept, &expected_kept) &&
          racks_are_equal(&exchanged, &expected_exchanged)) {
        exchanges[exchange_idx].sampled_count++;
        found = true;
        break;
      }
    }
    assert(found);
  }
  for (int exchange_idx = 0; exchange_idx < num_exchanges; exchange_idx++) {
    const AliasMethodTestExchange *exchange = &exchanges[exchange_idx];
    const double expected =
        (weighted ? exchange->weight : (double)exchange->count) / total;
    const double sampled =
        (double)exchange->sampled_count / NUM_EXCHANGE_SAMPLES;
    // More than five standard errors at this many samples.
    assert(fabs(sampled - expected) < 0.02);
  }
  prng_destroy(forward_prng);
  prng_destroy(backward_prng);
  prng_destroy(kept_only_prng);
  alias_method_destroy(forward);
  alias_method_destroy(backward);
}

// Adds an exchange for every tile kept and every pair of tiles thrown back
// with another copy of it, last to first when reversed.
static void add_exchanges_past_initial_capacity(const LetterDistribution *ld,
                                                AliasMethod *am,
                                                const bool reversed) {
  const int num_exchanges =
      NUM_EXCHANGE_LETTERS * NUM_EXCHANGE_LETTERS * NUM_EXCHANGE_LETTERS;
  Rack kept;
  Rack exchanged;
  rack_set_dist_size_and_reset(&kept, ld_get_size(ld));
  rack_set_dist_size_and_reset(&exchanged, ld_get_size(ld));
  for (int add_idx = 0; add_idx < num_exchanges; add_idx++) {
    const int exchange_idx = reversed ? num_exchanges - 1 - add_idx : add_idx;
    const MachineLetter kept_ml =
        (MachineLetter)(1 + exchange_idx % NUM_EXCHANGE_LETTERS);
    const MachineLetter first_ml =
        (MachineLetter)(1 + exchange_idx / NUM_EXCHANGE_LETTERS %
                                NUM_EXCHANGE_LETTERS);
    const MachineLetter second_ml =
        (MachineLetter)(1 + exchange_idx /
                                (NUM_EXCHANGE_LETTERS * NUM_EXCHANGE_LETTERS));
    if (first_ml > second_ml) {
      continue;
    }
    rack_reset(&kept);
    rack_add_letter(&kept, kept_ml);
    rack_reset(&exchanged);
    rack_add_letter(&exchanged, kept_ml);
    rack_add_letter(&exchanged, first_ml);
    rack_add_letter(&exchanged, second_ml);
    alias_method_add_exchange(am, &kept, &exchanged, 1 + first_ml % 3);
  }
}

// A table that grows past its initial capacity keeps every item's exchanged
// tiles with it through the sort, in an order that does not depend on the
// order the items were added in. A reset table holds no exchanged tiles, and
// can hold them again.
static void test_alias_method_exchange_growth(const Config *config) {
  const LetterDistribution *ld = config_get_ld(config);
  AliasMethod *forward = alias_method_create();
  AliasMethod *backward = alias_method_create();
  Rack kept;
  Rack exchanged;
  rack_set_dist_size_and_reset(&kept, ld_get_size(ld));
  rack_set_dist_size_and_reset(&exchanged, ld_get_size(ld));
  XoshiroPRNG *prng = prng_create(7);
  for (int round_idx = 0; round_idx < 2; round_idx++) {
    add_exchanges_past_initial_capacity(ld, forward, false);
    add_exchanges_past_initial_capacity(ld, backward, true);
    assert(forward->num_items > 8 * INITIAL_AM_ENTRIES_SIZE);
    assert(forward->num_items == backward->num_items);
    const bool forward_generated = alias_method_generate_tables(forward);
    assert(forward_generated);
    const bool backward_generated = alias_method_generate_tables(backward);
    assert(backward_generated);
    for (uint32_t item_idx = 0; item_idx < forward->num_items; item_idx++) {
      const AliasMethodItem *item = &forward->items[item_idx];
      const AliasMethodItem *backward_item = &backward->items[item_idx];
      assert(memcmp(&item->rack, &backward_item->rack, sizeof(item->rack)) ==
             0);
      assert(item->count == backward_item->count);
      assert(item->alias == backward_item->alias);
      assert(item->probability == backward_item->probability);
      assert(memcmp(&forward->exchanged_racks[item_idx],
                    &backward->exchanged_racks[item_idx],
                    sizeof(EncodedRack)) == 0);
      rack_decode(&item->rack, &kept);
      rack_decode(&forward->exchanged_racks[item_idx], &exchanged);
      assert(rack_get_total_letters(&kept) == 1);
      assert(rack_get_total_letters(&exchanged) == 3);
      for (int ml = 0; ml < ld_get_size(ld); ml++) {
        assert(rack_get_letter(&kept, ml) <= rack_get_letter(&exchanged, ml));
      }
    }
    alias_method_reset(forward);
    alias_method_reset(backward);
    assert(!alias_method_has_exchanged_racks(forward));
    rack_set_to_string(ld, &kept, "AE");
    alias_method_add_rack(forward, &kept, 1);
    const bool plain_generated = alias_method_generate_tables(forward);
    assert(plain_generated);
    assert(!alias_method_has_exchanged_racks(forward));
    assert(alias_method_sample(forward, prng, &exchanged));
    assert(racks_are_equal(&kept, &exchanged));
    alias_method_reset(forward);
  }
  prng_destroy(prng);
  alias_method_destroy(forward);
  alias_method_destroy(backward);
}

void test_alias_method(void) {
  Config *config =
      config_create_or_die("set -lex CSW21 -s1 equity -s2 equity -r1 all -r2 "
                           "all -numplays 1  -gp false -threads 1");
  AliasMethod *am = alias_method_create();
  XoshiroPRNG *prng = prng_create(0);

  const bool empty_generated = alias_method_generate_tables(am);

  assert(!empty_generated);
  assert(!alias_method_sample(am, prng, NULL));

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
  prng_destroy(prng);
  test_alias_method_order_independent(config);
  test_alias_method_zero_weight(config);
  test_alias_method_exchange_order_independent(config, false);
  test_alias_method_exchange_order_independent(config, true);
  test_alias_method_exchange_growth(config);
  config_destroy(config);
}
