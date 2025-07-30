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

void download_gcg(const DownloadGCGOptions *options, GameHistory *game_history,
                  ErrorStack *error_stack) {
  if (!options || !options->source_identifier || !game_history) {
    error_stack_push(error_stack, ERROR_STATUS_CONFIG_LOAD_MISSING_ARG,
                     string_duplicate("Invalid arguments to download_gcg"));
    return;
  }

  const char *identifier = options->source_identifier;
  char *gcg_content = NULL;

  // Check what type of identifier we have
  bool is_cross_tables_url = strstr(identifier, "cross-tables.com") != NULL;
  bool is_woogles_url = strstr(identifier, "woogles.io") != NULL;
  bool is_numeric_game_id = true;
  
  
  // Check if identifier is all digits (potential game ID)
  if (!is_cross_tables_url && !is_woogles_url) {
    for (int i = 0; identifier[i]; i++) {
      if (!isdigit(identifier[i])) {
        is_numeric_game_id = false;
        break;
      }
    }
  }
  
  if (is_cross_tables_url || (is_numeric_game_id && !is_woogles_url)) {
    // Extract game ID from URL or use provided ID
    char *game_id_str = NULL;
    
    if (is_cross_tables_url) {
      // Extract game ID from URL parameter u=XXXXX
      const char *u_param = strstr(identifier, "u=");
      if (u_param) {
        u_param += 2; // Skip "u="
        const char *end = u_param; 
        while (*end && *end != '&' && *end != '#') {
          end++;
        }
        int id_len = end - u_param;
        game_id_str = malloc_or_die(id_len + 1);
        strncpy(game_id_str, u_param, id_len);
        game_id_str[id_len] = '\0';
      }
    } else {
      // Use the identifier as game ID
      game_id_str = string_duplicate(identifier);
    }
    
    if (game_id_str) {
      // Build the actual GCG URL: ./annotated/selfgcg/[first3]/anno[full_id].gcg
      StringBuilder *url_builder = string_builder_create();
      
      // Get first 3 digits for the path
      char first_three[4] = {0};
      for (int i = 0; i < 3 && game_id_str[i]; i++) {
        first_three[i] = game_id_str[i];
      }
      
      string_builder_add_formatted_string(url_builder, 
                                          "https://cross-tables.com/annotated/selfgcg/%s/anno%s.gcg", 
                                          first_three, game_id_str);
      char *gcg_url = string_builder_dump(url_builder, NULL);
      string_builder_destroy(url_builder);
      
      
      if (gcg_url) {
        // Use curl to download the GCG content
        StringBuilder *cmd_builder = string_builder_create();
        string_builder_add_formatted_string(cmd_builder, "curl -s -L \"%s\"", gcg_url);
        char *curl_cmd = string_builder_dump(cmd_builder, NULL);
        string_builder_destroy(cmd_builder);

        FILE *pipe = popen(curl_cmd, "r");
        free(curl_cmd);
        free(gcg_url);

        if (pipe) {
          StringBuilder *content_builder = string_builder_create();
          char buffer[4096];
          while (fgets(buffer, sizeof(buffer), pipe) != NULL) {
            string_builder_add_string(content_builder, buffer);
          }
          
          int status = pclose(pipe);
          if (status == 0) {
            gcg_content = string_builder_dump(content_builder, NULL);
          }
          string_builder_destroy(content_builder);
        }
      }
      
      free(game_id_str);
    }

    if (!gcg_content || is_string_empty_or_whitespace(gcg_content)) {
      free(gcg_content);
      error_stack_push(error_stack, ERROR_STATUS_FILEPATH_FILE_NOT_FOUND,
                       get_formatted_string("Failed to download GCG from cross-tables: %s",
                                            identifier));
      return;
    }
  }
  else if (is_woogles_url) {
    // Extract game ID from woogles URL
    char *game_id_str = NULL;
    
    // Look for game ID in URL - typically in format like /game/[game_id] or ?gameID=[game_id]
    const char *game_param = strstr(identifier, "/game/");
    if (game_param) {
      game_param += 6; // Skip "/game/"
      const char *end = game_param;
      while (*end && *end != '?' && *end != '/' && *end != '#') {
        end++;
      }
      int id_len = end - game_param;
      game_id_str = malloc_or_die(id_len + 1);
      strncpy(game_id_str, game_param, id_len);
      game_id_str[id_len] = '\0';
    } else {
      // Try to find gameID parameter
      const char *game_id_param = strstr(identifier, "gameID=");
      if (game_id_param) {
        game_id_param += 7; // Skip "gameID="
        const char *end = game_id_param;
        while (*end && *end != '&' && *end != '#') {
          end++;
        }
        int id_len = end - game_id_param;
        game_id_str = malloc_or_die(id_len + 1);
        strncpy(game_id_str, game_id_param, id_len);
        game_id_str[id_len] = '\0';
      }
    }
    
    if (game_id_str) {
      // Use woogles API to get GCG content
      StringBuilder *json_builder = string_builder_create();
      string_builder_add_formatted_string(json_builder, "{\"game_id\":\"%s\"}", game_id_str);
      char *json_payload = string_builder_dump(json_builder, NULL);
      string_builder_destroy(json_builder);
      
      StringBuilder *cmd_builder = string_builder_create();
      string_builder_add_string(cmd_builder, "curl -s ");
      string_builder_add_string(cmd_builder, "-H 'Content-Type: application/json' ");
      string_builder_add_string(cmd_builder, "https://woogles.io/api/game_service.GameMetadataService/GetGCG ");
      string_builder_add_string(cmd_builder, "-d '");
      string_builder_add_string(cmd_builder, json_payload);
      string_builder_add_string(cmd_builder, "'");
      char *curl_cmd = string_builder_dump(cmd_builder, NULL);
      string_builder_destroy(cmd_builder);
      
      
      FILE *pipe = popen(curl_cmd, "r");
      free(curl_cmd);
      free(json_payload);
      free(game_id_str);
      
      if (pipe) {
        StringBuilder *content_builder = string_builder_create();
        char buffer[4096];
        while (fgets(buffer, sizeof(buffer), pipe) != NULL) {
          string_builder_add_string(content_builder, buffer);
        }
        
        int status = pclose(pipe);
        if (status == 0) {
          char *response = string_builder_dump(content_builder, NULL);
          
          
          // Parse JSON response to extract GCG content
          // Look for "gcg" field in JSON response
          const char *gcg_start = strstr(response, "\"gcg\":\"");
          if (gcg_start) {
            gcg_start += 7; // Skip "\"gcg\":\""
            const char *gcg_end = gcg_start;
            while (*gcg_end && *gcg_end != '"') {
              gcg_end++;
            }
            int gcg_len = gcg_end - gcg_start;
            gcg_content = malloc_or_die(gcg_len + 1);
            strncpy(gcg_content, gcg_start, gcg_len);
            gcg_content[gcg_len] = '\0';
            
            // Unescape JSON string (convert \\n to \n, etc.)
            char *unescaped_gcg = malloc_or_die(gcg_len + 1);
            int write_pos = 0;
            for (int read_pos = 0; read_pos < gcg_len; read_pos++) {
              if (gcg_content[read_pos] == '\\' && read_pos + 1 < gcg_len) {
                if (gcg_content[read_pos + 1] == 'n') {
                  unescaped_gcg[write_pos++] = '\n';
                  read_pos++; // Skip the 'n'
                } else if (gcg_content[read_pos + 1] == 't') {
                  unescaped_gcg[write_pos++] = '\t';
                  read_pos++; // Skip the 't'
                } else if (gcg_content[read_pos + 1] == '\\') {
                  unescaped_gcg[write_pos++] = '\\';
                  read_pos++; // Skip the second backslash
                } else if (gcg_content[read_pos + 1] == '"') {
                  unescaped_gcg[write_pos++] = '"';
                  read_pos++; // Skip the quote
                } else {
                  unescaped_gcg[write_pos++] = gcg_content[read_pos];
                }
              } else {
                unescaped_gcg[write_pos++] = gcg_content[read_pos];
              }
            }
            unescaped_gcg[write_pos] = '\0';
            
            free(gcg_content);
            gcg_content = unescaped_gcg;
          }
          
          free(response);
        }
        string_builder_destroy(content_builder);
      }
    }
    
    if (!gcg_content || is_string_empty_or_whitespace(gcg_content)) {
      free(gcg_content);
      error_stack_push(error_stack, ERROR_STATUS_FILEPATH_FILE_NOT_FOUND,
                       get_formatted_string("Failed to download GCG from woogles.io: %s",
                                            identifier));
      return;
    }
  }
  else {
    // Assume it's a local file path
    FILE *file = fopen(identifier, "r");
    if (!file) {
      error_stack_push(error_stack, ERROR_STATUS_FILEPATH_FILE_NOT_FOUND,
                       get_formatted_string("Failed to open local file: %s", identifier));
      return;
    }
    
    // Read entire file content
    StringBuilder *content_builder = string_builder_create();
    char buffer[4096];
    while (fgets(buffer, sizeof(buffer), file) != NULL) {
      string_builder_add_string(content_builder, buffer);
    }
    
    fclose(file);
    gcg_content = string_builder_dump(content_builder, NULL);
    string_builder_destroy(content_builder);
    
    if (!gcg_content || is_string_empty_or_whitespace(gcg_content)) {
      free(gcg_content);
      error_stack_push(error_stack, ERROR_STATUS_FILEPATH_FILE_NOT_FOUND,
                       get_formatted_string("Local file is empty or unreadable: %s", identifier));
      return;
    }
  }

  // Parse the GCG content using provided config (required)
  Config *config_to_use = (Config*)options->config;
  
  if (!config_to_use) {
    // No config provided - return success for download but indicate parsing needs config
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