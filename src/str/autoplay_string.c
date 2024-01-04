#include <stdlib.h>

#include "../ent/autoplay_results.h"
#include "../ent/thread_control.h"

#include "../util/string_util.h"

void print_ucgi_autoplay_results(const AutoplayResults *autoplay_results,
                                 ThreadControl *thread_control) {
  char *results_string = get_formatted_string(
      "autoplay %d %d %d %d %d %f %f %f %f\n",
      autoplay_results_get_games(autoplay_results), autoplay_results_get_p1_wins(autoplay_results),
      autoplay_results_get_p1_losses(autoplay_results), autoplay_results_get_p1_ties(autoplay_results),
      autoplay_results_get_p1_firsts(autoplay_results), stat_get_mean(autoplay_results_get_p1_score(autoplay_results)),
      stat_get_stdev(autoplay_results_get_p1_score(autoplay_results)),
      stat_get_mean(autoplay_results_get_p2_score(autoplay_results)),
      stat_get_stdev(autoplay_results_get_p2_score(autoplay_results)));
  thread_control_print(thread_control, results_string);
  free(results_string);
}