#include <stdlib.h>

#include "../ent/autoplay_results.h"
#include "../ent/thread_control.h"

#include "../util/string_util.h"

void print_ucgi_autoplay_results(const AutoplayResults *autoplay_results,
                                 ThreadControl *thread_control) {
  char *results_string = get_formatted_string(
      "autoplay %d %d %d %d %d %f %f %f %f\n",
      get_total_games(autoplay_results), get_p1_wins(autoplay_results),
      get_p1_losses(autoplay_results), get_p1_ties(autoplay_results),
      get_p1_firsts(autoplay_results), get_mean(get_p1_score(autoplay_results)),
      get_stdev(get_p1_score(autoplay_results)),
      get_mean(get_p2_score(autoplay_results)),
      get_stdev(get_p2_score(autoplay_results)));
  print_to_outfile(thread_control, results_string);
  free(results_string);
}