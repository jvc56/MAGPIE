#include "load.h"

#include "../ent/game_history.h"
#include "../util/io_util.h"
#include "../util/string_util.h"
#include "config.h"
#include "gcg.h"
#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <regex.h>
#include <unistd.h>

#define WOOGLES_GCG_START "\"gcg\":\""

char *get_xt_gcg_string(const char *identifier, ErrorStack *error_stack) {
  char *game_id_str = NULL;

  // Check if this is a Cross-tables URL
  if (strstr(identifier, "cross-tables.com") != NULL) {
      // Extract game ID from URL parameter u=XXXXX using regex
      regex_t regex;
      regmatch_t matches[2];
      
      int regex_status = regcomp(&regex, "u=([0-9]+)", REG_EXTENDED);
      if (regex_status != 0) {
          log_fatal("Failed to compile regex for cross-tables URL parsing");
      }
      
      int match_result = regexec(&regex, identifier, 2, matches, 0);
      if (match_result == 0) {
          int start = matches[1].rm_so;
          int end = matches[1].rm_eo;
          int id_len = end - start;
          
          game_id_str = malloc_or_die(id_len + 1);
          strncpy(game_id_str, identifier + start, id_len);
          game_id_str[id_len] = '\0';
      } else {
          regfree(&regex);
          error_stack_push(error_stack, ERROR_STATUS_XT_URL_MALFORMED,
                          get_formatted_string("Failed to extract game ID from cross-tables URL: %s", identifier));
          return NULL;
      }
      
      regfree(&regex);
  } else {
      // Check if it's a numeric game id
      if (!is_all_digits_or_empty(identifier)){
        return NULL;
      }
      game_id_str = string_duplicate(identifier);
  }

    char *gcg_content = NULL;

    // Get first 3 digits for the path
    char first_three[4] = {0};
    for (int i = 0; i < 3 && game_id_str[i]; i++) {
        first_three[i] = game_id_str[i];
    }

    // Build the curl command directly
    char *curl_cmd = get_formatted_string("curl -s -L \"https://cross-tables.com/annotated/selfgcg/%s/anno%s.gcg\"", 
                                        first_three, game_id_str);

    gcg_content = get_process_output(curl_cmd);

    free(curl_cmd);
    free(game_id_str);  

  if (!is_valid_gcg_content(gcg_content)) {
      free(gcg_content);
        error_stack_push(error_stack, ERROR_STATUS_BAD_XT_GCG,
                        get_formatted_string("Invalid or missing GCG content from cross-tables ID: %s", identifier));
      return NULL;
  }
  
  return gcg_content;
}

char *get_woogles_gcg_string(const char *identifier, ErrorStack *error_stack) {
    char *game_id_str = NULL;

    // Check if this is a Woogles URL
    if (strstr(identifier, "woogles.io") != NULL) {
        // Extract game ID from Woogles URL using regex
        regex_t regex;
        regmatch_t matches[2];

        // Pattern matches /game/[alphanumeric]
        int regex_status = regcomp(&regex, "/game/([a-zA-Z0-9]+)", REG_EXTENDED);
        if (regex_status != 0) {
            log_fatal("Failed to compile regex for woogles URL parsing");
        }

        int match_result = regexec(&regex, identifier, 2, matches, 0);
        if (match_result == 0) {
            int start = matches[1].rm_so;
            int end = matches[1].rm_eo;
            int id_len = end - start;
            
            game_id_str = malloc_or_die(id_len + 1);
            strncpy(game_id_str, identifier + start, id_len);
            game_id_str[id_len] = '\0';
        } else {
            regfree(&regex);
            error_stack_push(error_stack, ERROR_STATUS_WOOGLES_URL_MALFORMED,
                            get_formatted_string("Failed to extract game ID from woogles URL: %s", identifier));
            return NULL;
        }

        regfree(&regex);
    } else {
        // Check if it's a standalone woogles game ID (must be alphanumeric only)
        if (is_all_digits_or_empty(identifier)) {
            // All digits likely means cross-tables, not woogles
            return NULL;
        }
        // Check if identifier is alphanumeric only
        for (int i = 0; identifier[i]; i++) {
            if (!isalnum(identifier[i])) {
                // Contains non-alphanumeric characters, not a woogles game ID
                return NULL;
            }
        }
        // It's alphanumeric and not all digits, assume it's a woogles game ID
        game_id_str = string_duplicate(identifier);
    }
    
    char *gcg_content = NULL;

    // Use woogles API to get GCG content
    char *curl_cmd = get_formatted_string("curl -s -H 'Content-Type: application/json' "
                                            "https://woogles.io/api/game_service.GameMetadataService/GetGCG "
                                            "-d '{\"game_id\":\"%s\"}'", game_id_str);
    
    char *response = get_process_output(curl_cmd);
    free(curl_cmd);
    free(game_id_str);
    
    if (!response) {
        error_stack_push(error_stack, ERROR_STATUS_WOOGLES_URL_MALFORMED,
                        get_formatted_string("Failed to get response from woogles API for ID: %s", identifier));
        return NULL;
    }
    
    // Parse JSON response to extract GCG content
    const char *gcg_start = strstr(response, WOOGLES_GCG_START);
    if (!gcg_start) {
        error_stack_push(error_stack, ERROR_STATUS_BAD_WOOGLES_GCG,
                        get_formatted_string("Invalid or missing GCG content from woogles URL: %s", identifier));
    }
    gcg_start += string_length(WOOGLES_GCG_START);
    const char *gcg_end = gcg_start;
    while (*gcg_end && *gcg_end != '"') {
        gcg_end++;
    }
    int gcg_len = gcg_end - gcg_start;
    char *raw_gcg = malloc_or_die(gcg_len + 1);
    strncpy(raw_gcg, gcg_start, gcg_len);
    raw_gcg[gcg_len] = '\0';
    
    // Unescape JSON string using utility function
    gcg_content = json_unescape_string(raw_gcg);
    free(raw_gcg);
    
    
    free(response);

    if (!is_valid_gcg_content(gcg_content)) {
        free(gcg_content);
        error_stack_push(error_stack, ERROR_STATUS_BAD_WOOGLES_GCG,
                        get_formatted_string("Invalid or missing GCG content from woogles ID: %s",
                                                identifier));
        return NULL;
    }
    
    return gcg_content;
}

char *get_local_gcg_string(const char *identifier, ErrorStack *error_stack) {
    
    // Check if file exists and is readable
    if (access(identifier, R_OK) != 0) {
        return NULL;  // Not a valid local file - not our job
    }

    if (!error_stack_is_empty(error_stack)) {
        return NULL;
    }

    // Read file content directly
    char *gcg_content = get_string_from_file(identifier, error_stack);

    if (!is_valid_gcg_content(gcg_content)) {
        free(gcg_content);
        error_stack_push(error_stack, ERROR_STATUS_BAD_LOCAL_GCG,
                         get_formatted_string("Local file does not contain valid GCG content: %s", identifier));
        return NULL;
    }

    return gcg_content;
}

char *get_gcg_string(const DownloadGCGOptions *options, ErrorStack *error_stack) {
  
    if (!options || !options->source_identifier) {
        log_fatal("Invalid options or missing source_identifier passed to get_gcg_string");
    }
    const char *identifier = options->source_identifier;
    
    // Try Cross-tables first 
    char *gcg_string = get_xt_gcg_string(identifier, error_stack);
    if (!error_stack_is_empty(error_stack)) {
        return NULL;  // Fatal error occurred
    }
    if (gcg_string) {
        return gcg_string;
    }

    // Try Woogles
    gcg_string = get_woogles_gcg_string(identifier, error_stack);
    if (!error_stack_is_empty(error_stack)) {
        return NULL;  // Fatal error occurred
    }
    if (gcg_string) {
        return gcg_string;
    }

    // Try local file
    gcg_string = get_local_gcg_string(identifier, error_stack);
    if (!error_stack_is_empty(error_stack)) {
        return NULL;  // Fatal error occurred
    }
    if (gcg_string) {
        return gcg_string;
    }

    // If we get here, nothing worked
    error_stack_push(error_stack, ERROR_STATUS_FILEPATH_FILE_NOT_FOUND,
                     get_formatted_string("Could not load GCG from any source: %s", 
                                          options->source_identifier));
    return NULL;
}

void download_gcg(const DownloadGCGOptions *options, GameHistory *game_history, ErrorStack *error_stack) {
    if (!options || !game_history) {
        log_fatal("Missing options or game history");
    }

    // Get the GCG content from any available source
    char *gcg_content = get_gcg_string(options, error_stack);
    if (!gcg_content) {
        return;  // Error already pushed to stack by get_gcg_string
    }

    Config *config_to_use = options->config;
    if (!config_to_use) {
        error_stack_push(error_stack, ERROR_STATUS_CONFIG_LOAD_MISSING_ARG,
                         string_duplicate("GCG content downloaded successfully but parsing requires a valid config"));
        free(gcg_content);
        return;
    }

    // Parse the GCG content using the provided parser
    parse_gcg_string(gcg_content, config_to_use, game_history, error_stack);

    // Clean up
    free(gcg_content);
}