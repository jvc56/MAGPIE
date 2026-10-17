#include "../src/compat/cterm.h"
#include "../src/impl/exec.h"

int main(int argc, char *argv[]) {
  cterm_init();
  process_command_default(argc, argv);
}