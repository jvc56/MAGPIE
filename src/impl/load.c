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

char *get_xt_gcg_string(const char *identifier, ErrorStack *error_stack) {
  char *game_id_str = NULL;

  // Check if this is a Cross-tables URL
  if (strstr(identifier, "cross-tables.com") != NULL) {
      // Extract game ID from URL parameter u=XXXXX using regex
      regex_t regex;
      regmatch_t matches[2];
      
      regcomp(&regex, "u=([0-9]+)", REG_EXTENDED);
      
      int match_result = regexec(&regex, identifier, 2, matches, 0);
      if (match_result == 0) {
          int start = matches[1].rm_so;
          int end = matches[1].rm_eo;
          int id_len = end - start;
          
          game_id_str = malloc_or_die(id_len + 1);
          strncpy(game_id_str, identifier + start, id_len);
          game_id_str[id_len] = '\0';
      }
      
      regfree(&regex);
  } else {
      // Assume it's a numeric game ID
      game_id_str = string_duplicate(identifier);
  }

  char *gcg_content = NULL;

  if (game_id_str) {
      // Get first 3 digits for the path
      char first_three[4] = {0};
      for (int i = 0; i < 3 && game_id_str[i]; i++) {
          first_three[i] = game_id_str[i];
      }
      
      // Build the actual GCG URL and download
      char *gcg_url = get_formatted_string("https://cross-tables.com/annotated/selfgcg/%s/anno%s.gcg", 
                                          first_three, game_id_str);
      char *curl_cmd = get_formatted_string("curl -s -L \"%s\"", gcg_url);
      
      gcg_content = get_process_output(curl_cmd);
      
      free(curl_cmd);
      free(gcg_url);
      free(game_id_str);
  }

  if (!gcg_content || is_string_empty_or_whitespace(gcg_content) || 
      strstr(gcg_content, "<html>") != NULL || strstr(gcg_content, "File not found") != NULL) {
      free(gcg_content);
      // Only push error if this was actually a cross-tables URL
      if (strstr(identifier, "cross-tables.com") != NULL) {
          error_stack_push(error_stack, ERROR_STATUS_FILEPATH_FILE_NOT_FOUND,
                          get_formatted_string("Failed to download GCG from cross-tables: %s",
                                                identifier));
      }
      return NULL;
  }
  
  return gcg_content;
}

char *get_woogles_gcg_string(const char *identifier, ErrorStack *error_stack) {
    char *game_id_str = NULL;

    // Extract game ID from Woogles URL using regex
    regex_t regex;
    regmatch_t matches[2];

    // Pattern matches /game/[alphanumeric]
    regcomp(&regex, "/game/([a-zA-Z0-9]+)", REG_EXTENDED);

    int match_result = regexec(&regex, identifier, 2, matches, 0);
    if (match_result == 0) {
        int start = matches[1].rm_so;
        int end = matches[1].rm_eo;
        int id_len = end - start;
        
        game_id_str = malloc_or_die(id_len + 1);
        strncpy(game_id_str, identifier + start, id_len);
        game_id_str[id_len] = '\0';
    }

    regfree(&regex);
    
    char *gcg_content = NULL;

    if (game_id_str) {
        // Use woogles API to get GCG content
        char *json_payload = get_formatted_string("{\"game_id\":\"%s\"}", game_id_str);
        
        char *curl_cmd = get_formatted_string("curl -s -H 'Content-Type: application/json' "
                                              "https://woogles.io/api/game_service.GameMetadataService/GetGCG "
                                              "-d '%s'", json_payload);
        
        char *response = get_process_output(curl_cmd);
        free(curl_cmd);
        free(json_payload);
        free(game_id_str);
        
        if (response) {
            // Parse JSON response to extract GCG content
            const char *gcg_start = strstr(response, "\"gcg\":\"");
            if (gcg_start) {
                gcg_start += 7; // Skip "\"gcg\":\""
                const char *gcg_end = gcg_start;
                while (*gcg_end && *gcg_end != '"') {
                    gcg_end++;
                }
                int gcg_len = gcg_end - gcg_start;
                char *raw_gcg = malloc_or_die(gcg_len + 1);
                strncpy(raw_gcg, gcg_start, gcg_len);
                raw_gcg[gcg_len] = '\0';
                
                // Unescape JSON string
                char *unescaped_gcg = malloc_or_die(gcg_len + 1);
                char *src = raw_gcg;
                char *dst = unescaped_gcg;
                
                regex_t unescape_regex;
                regcomp(&unescape_regex, "\\\\(.)", REG_EXTENDED);
                
                regmatch_t unescape_matches[2];
                int offset = 0;
                
                while (regexec(&unescape_regex, src + offset, 2, unescape_matches, 0) == 0) {
                    // Copy everything before the match
                    int chars_before = unescape_matches[0].rm_so;
                    strncpy(dst, src + offset, chars_before);
                    dst += chars_before;
                    
                    // Handle the escaped character
                    char escaped_char = src[offset + unescape_matches[1].rm_so];
                    switch (escaped_char) {
                        case 'n': *dst++ = '\n'; break;
                        case 't': *dst++ = '\t'; break;
                        case '\\': *dst++ = '\\'; break;
                        case '"': *dst++ = '"'; break;
                        default: *dst++ = escaped_char; break;
                    }
                    
                    // Move past this match
                    offset += unescape_matches[0].rm_eo;
                }
                
                // Copy any remaining characters after the last match
                strcpy(dst, src + offset);
                
                regfree(&unescape_regex);
                free(raw_gcg);
                gcg_content = unescaped_gcg;
            }
            
            free(response);
        }
    }
    
    if (!gcg_content || is_string_empty_or_whitespace(gcg_content) ||
        strstr(gcg_content, "<html>") != NULL || strstr(gcg_content, "File not found") != NULL) {
        free(gcg_content);
        // Only push error if this was actually a woogles URL
        if (strstr(identifier, "woogles.io") != NULL) {
            error_stack_push(error_stack, ERROR_STATUS_FILEPATH_FILE_NOT_FOUND,
                            get_formatted_string("Failed to download GCG from woogles.io: %s",
                                                  identifier));
        }
        return NULL;
    }
    
    return gcg_content;
}

char *get_local_gcg_string(const char *identifier, ErrorStack *error_stack) {
    
    // Try to open as local file
    FILE *file = fopen(identifier, "r");
    if (!file) {
        return NULL;  // Not a valid local file - not our job
    }
    fclose(file);
    
    // Read file content using cat command
    char *cat_cmd = get_formatted_string("cat \"%s\"", identifier);
    char *gcg_content = get_process_output(cat_cmd);
    free(cat_cmd);

    if (!gcg_content || is_string_empty_or_whitespace(gcg_content)) {
        free(gcg_content);
        error_stack_push(error_stack, ERROR_STATUS_FILEPATH_FILE_NOT_FOUND,
                         get_formatted_string("Local file is empty or unreadable: %s", identifier));
        return NULL;
    }

    return gcg_content;
}

char *get_gcg_string(const DownloadGCGOptions *options, ErrorStack *error_stack) {
  
    if (!options || !options->source_identifier) {
        error_stack_push(error_stack, ERROR_STATUS_CONFIG_LOAD_MISSING_ARG,
                         string_duplicate("Invalid options or missing source_identifier"));
        return NULL;
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
        error_stack_push(error_stack, ERROR_STATUS_CONFIG_LOAD_MISSING_ARG,
                         string_duplicate("Invalid arguments to download_gcg"));
        return;
    }

    // Get the GCG content from any available source
    char *gcg_content = get_gcg_string(options, error_stack);
    if (!gcg_content) {
        return;  // Error already pushed to stack by get_gcg_string
    }

    // Validate config is provided for parsing
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