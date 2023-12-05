#ifndef THREAD_CONTROL_H
#define THREAD_CONTROL_H

#include "../def/thread_control_defs.h"

#include "file_handler.h"

struct ThreadControl;
typedef struct ThreadControl ThreadControl;

ThreadControl *create_thread_control();
void destroy_thread_control(ThreadControl *thread_control);
bool halt(ThreadControl *thread_control, halt_status_t halt_status);
bool unhalt(ThreadControl *thread_control);
bool is_halted(ThreadControl *thread_control);
FileHandler *get_infile(ThreadControl *thread_control);
Timer *get_timer(ThreadControl *thread_control);
halt_status_t get_halt_status(ThreadControl *thread_control);
int get_print_info_interval(ThreadControl *thread_control);
void set_print_info_interval(ThreadControl *thread_control,
                             int print_info_interval);
int get_check_stopping_condition_interval(ThreadControl *thread_control);
void set_check_stopping_condition_interval(
    ThreadControl *thread_control, int check_stopping_condition_interval);
bool set_mode_searching(ThreadControl *thread_control);
bool set_mode_stopped(ThreadControl *thread_control);
mode_search_status_t get_mode(ThreadControl *thread_control);
void set_io(ThreadControl *thread_control, const char *in_filename,
            const char *out_filename);
bool set_check_stop_active(ThreadControl *thread_control);
bool set_check_stop_inactive(ThreadControl *thread_control);
int get_number_of_threads(ThreadControl *thread_control);
void set_number_of_threads(ThreadControl *thread_control,
                           int number_of_threads);
void print_to_outfile(ThreadControl *thread_control, const char *content);
void wait_for_mode_stopped(ThreadControl *thread_control);

#endif