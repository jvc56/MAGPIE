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
  
  // Check if this is a Cross-tables URL first
  regex_t xt_regex;
  int xt_regex_status = regcomp(&xt_regex, "cross-tables\\.com/annotated\\.php\\?u=([0-9]+)", REG_EXTENDED);
  if (xt_regex_status == 0) {
      regmatch_t xt_matches[2];
      int xt_match_result = regexec(&xt_regex, identifier, 2, xt_matches, 0);
      regfree(&xt_regex);
      
      if (xt_match_result == 0) {
          // Extract game ID from the already matched regex
          int start = xt_matches[1].rm_so;
          int end = xt_matches[1].rm_eo;
          int id_len = end - start;
          
          game_id_str = malloc_or_die(id_len + 1);
          strncpy(game_id_str, identifier + start, id_len);
          game_id_str[id_len] = '\0';
      } else {
          // Check if it's a numeric game id
          if (!is_all_digits_or_empty(identifier)){
            return NULL;
          }
          game_id_str = string_duplicate(identifier);
      }
  } else {
      // Regex compilation failed, fallback to numeric check
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

    // Check if this is a Woogles URL first
    regex_t woogles_regex;
    int woogles_regex_status = regcomp(&woogles_regex, "woogles\\.io/game/([a-zA-Z0-9]+)", REG_EXTENDED);
    if (woogles_regex_status == 0) {
        regmatch_t woogles_matches[2];
        int woogles_match_result = regexec(&woogles_regex, identifier, 2, woogles_matches, 0);
        regfree(&woogles_regex);
        
        if (woogles_match_result == 0) {
            // Extract game ID from the already matched regex
            int start = woogles_matches[1].rm_so;
            int end = woogles_matches[1].rm_eo;
            int id_len = end - start;
            
            game_id_str = malloc_or_die(id_len + 1);
            strncpy(game_id_str, identifier + start, id_len);
            game_id_str[id_len] = '\0';
        } else {
            // Check if it's a standalone woogles game ID (alphanumeric, not all digits)
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

char *get_url_gcg_string(const char *identifier, ErrorStack *error_stack){
    // Check if this looks like a URL (http, https, ftp, sftp)
    regex_t url_regex;
    int regex_status = regcomp(&url_regex, "^(https?|s?ftp)://", REG_EXTENDED);
    if (regex_status != 0) {
        return NULL; // If regex fails, skip URL processing
    }
    
    int match_result = regexec(&url_regex, identifier, 0, NULL, 0);
    regfree(&url_regex);
    
    if (match_result == 0) {
        // It's a URL - try to download directly
        char *curl_cmd = get_formatted_string("curl -s -L \"%s\"", identifier);
        char *gcg_content = get_process_output(curl_cmd);
        free(curl_cmd);
        
        if (!gcg_content) {
            error_stack_push(error_stack, ERROR_STATUS_UNRECOGNIZED_URL,
                            get_formatted_string("Failed to get response from URL: %s", identifier));
            return NULL;
        }
        
        if (!is_valid_gcg_content(gcg_content)) {
            free(gcg_content);
            error_stack_push(error_stack, ERROR_STATUS_UNRECOGNIZED_URL,
                            get_formatted_string("Invalid or missing GCG content from URL: %s", identifier));
            return NULL;
        }
        
        return gcg_content;
    }
    
    return NULL; // Not a URL
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

    if (!error_stack_is_empty(error_stack)) {
        return NULL;
    }

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
    
    // Try cross-tables first 
    char *gcg_string = get_xt_gcg_string(identifier, error_stack);
    if (!error_stack_is_empty(error_stack)) {
        return NULL; 
    }
    if (gcg_string) {
        return gcg_string;
    }

    // Try Woogles
    gcg_string = get_woogles_gcg_string(identifier, error_stack);
    if (!error_stack_is_empty(error_stack)) {
        return NULL;  
    }
    if (gcg_string) {
        return gcg_string;
    }

    // Try local file
    gcg_string = get_local_gcg_string(identifier, error_stack);
    if (!error_stack_is_empty(error_stack)) {
        return NULL; 
    }
    if (gcg_string) {
        return gcg_string;
    }

    // Try generic URL download
    gcg_string = get_url_gcg_string(identifier, error_stack);
    if (!error_stack_is_empty(error_stack)) {
        return NULL;  
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

    // Parse the GCG content using the provided parser
    parse_gcg_string(gcg_content, options->config, game_history, error_stack);

    // Clean up
    free(gcg_content);
}