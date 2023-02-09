#include <stdio.h>
#include <stdlib.h>
#include <getopt.h>

#include "../src/config.h"

#include "alphabet_test.h"
#include "bag_test.h"
#include "board_test.h"
#include "cross_set_test.h"
#include "equity_adjustment_test.h"
#include "game_test.h"
#include "gameplay_test.h"
#include "leaves_test.h"
#include "letter_distribution_test.h"
#include "movegen_test.h"
#include "rack_test.h"

void unit_tests() {
    // Test the readonly data first
    test_alphabet();
    test_leaves();
    test_letter_distribution();

    // Now test the rest
    test_bag();
    test_rack();
    test_board();
    test_cross_set();
    test_game();
    test_movegen();
    test_equity_adjustments();
    test_gameplay();
}

void print_all_moves_for_cgp() {

}

int main(int argc, char *argv[]) {
  int c;

  while (1)
    {
      static struct option long_options[] =
        {
          {"cgp",          required_argument,       0, 'c'},
          {"dist", required_argument, 0, 'd'},
          {"gaddag",       required_argument, 0, 'g'},
          {"laddag",       required_argument, 0, 'l'},
          {"conv",         required_argument, 0, 'v'},
          {0, 0, 0, 0}
        };
      int option_index = 0;
      c = getopt_long_only (argc, argv, "c:d:g:l:v:",
                       long_options, &option_index);

      /* Detect the end of the options. */
      if (c == -1)
        break;

      switch (c)
        {
        case 0:
          printf ("option %s", long_options[option_index].name);
          if (optarg)
            printf (" with arg %s", optarg);
          printf ("\n");
          break;

        case 'c':
          printf ("option -c with value `%s'\n", optarg);
          break;

        case 'd':
          printf ("option -d with value `%s'\n", optarg);
          break;

        case 'g':
          printf ("option -g with value `%s'\n", optarg);
          break;

        case 'l':
          printf ("option -l with value `%s'\n", optarg);
          break;

        case 'v':
          printf ("option -v with value `%s'\n", optarg);
          break;

        case '?':
          /* getopt_long already printed an error message. */
          break;

        default:
          abort ();
        }
    }

  /* Print any remaining command line arguments (not options). */
  if (optind < argc)
    {
      printf ("non-option ARGV-elements: ");
      while (optind < argc)
        printf ("%s ", argv[optind++]);
      putchar ('\n');
    }

  exit (0);
}