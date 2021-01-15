/* Copyright (C) 2015-2021, Wazuh Inc.
 * All rights reserved.
 *
 * This program is free software; you can redistribute it
 * and/or modify it under the terms of the GNU General Public
 * License (version 2) as published by the FSF - Free Software
 * Foundation.
 */

#include "state.h"
#include "shared.h"

#ifdef WAZUH_UNIT_TESTING
#define STATIC
#else
#define STATIC static
#endif

/* Global variables */
cJSON * g_lc_json_stats;           ///< JSON representation of states
lc_states_t * g_lc_states_global;   ///< global state struct storage
lc_states_t * g_lc_states_interval; ///< interval state struct storage
pthread_mutex_t g_lc_raw_stats_mutex = PTHREAD_MUTEX_INITIALIZER; ///< g_lc_states_* structs mutual exclusion mechanism
pthread_mutex_t g_lc_json_stats_mutex = PTHREAD_MUTEX_INITIALIZER; ///< g_lc_json_stats mutual exclusion mechanism

/**
 * @brief Trigger the generation of states
 *
 */
STATIC void w_logcollector_generate_state();

/**
 * @brief Generate and process the current states information
 *
 * @param state state to generate
 * @param restart restart counters and date after generating
 * @return cJSON * json decription with state information
 */
STATIC cJSON * _w_logcollector_generate_state(lc_states_t * state, bool restart);

/**
 * @brief Update/register current event and byte count for a particular file/location
 *
 * @param state state to be used
 * @param fpath file path or locafile location value
 * @param bytes amount of bytes
 */
STATIC void _w_logcollector_state_update_file(lc_states_t * state, char * fpath, uint64_t bytes);

/**
 * @brief Update/register current drop count for a target belonging to a particular file
 *
 * @param state state to be used
 * @param fpath file path or locafile location value
 * @param target target name
 * @param dropped true if want to register a drop.
 */
STATIC void _w_logcollector_state_update_target(lc_states_t * state, char * fpath, char * target, bool dropped);

/**
 * @brief Dump state information to file
 *
 */
STATIC void w_logcollector_state_dump();

#ifdef WIN32
DWORD WINAPI w_logcollector_state_main(__attribute__((unused)) void * args) {
#else
void * w_logcollector_state_main(__attribute__((unused)) void * args) {
#endif

    int interval = *(int *) args;

    while (FOREVER()) {
        sleep(interval);
        w_logcollector_generate_state();
        w_logcollector_state_dump();
    }
#ifndef WIN32
    return NULL;
#endif
}

STATIC void w_logcollector_state_dump() {

    cJSON * lc_state_json = w_logcollector_state_get();
    char * lc_state_str = cJSON_Print(lc_state_json);
    cJSON_Delete(lc_state_json);

    // Add trailing newline
    size_t len = strlen(lc_state_str);
    os_realloc(lc_state_str, len + 2, lc_state_str);
    lc_state_str[len] = '\n';
    lc_state_str[len + 1] = '\0';

    FILE * lc_state_file = NULL;

    if (lc_state_file = fopen(LOGCOLLECTOR_STATE_PATH, "w"), lc_state_file != NULL) {
        if (fwrite(lc_state_str, sizeof(char), len + 1, lc_state_file) < 1) {
            merror(FWRITE_ERROR, LOGCOLLECTOR_STATE_PATH, errno, strerror(errno));
        }
        fclose(lc_state_file);
    } else {
        merror(FOPEN_ERROR, LOGCOLLECTOR_STATE_PATH, errno, strerror(errno));
    }

    os_free(lc_state_str);
}

void w_logcollector_state_init() {

    os_calloc(1, sizeof(lc_states_t), g_lc_states_global);
    os_calloc(1, sizeof(lc_states_t), g_lc_states_interval);

    g_lc_states_global->start = time(NULL);
    g_lc_states_interval->start = time(NULL);

    if (g_lc_states_global->states = OSHash_Create(), g_lc_states_global->states == NULL) {
        merror_exit(HCREATE_ERROR, LOGCOLLECTOR_STATE_DESCRIPTION);
    }

    if (g_lc_states_interval->states = OSHash_Create(), g_lc_states_interval->states == NULL) {
        merror_exit(HCREATE_ERROR, LOGCOLLECTOR_STATE_DESCRIPTION);
    }

    if (OSHash_setSize(g_lc_states_global->states, LOGCOLLECTOR_STATE_FILES_MAX) == 0) {
        merror_exit(HSETSIZE_ERROR, LOGCOLLECTOR_STATE_DESCRIPTION);
    }

    if (OSHash_setSize(g_lc_states_interval->states, LOGCOLLECTOR_STATE_FILES_MAX) == 0) {
        merror_exit(HSETSIZE_ERROR, LOGCOLLECTOR_STATE_DESCRIPTION);
    }
}

void w_logcollector_state_update_target(char * fpath, char * target, bool dropped) {

    if (fpath == NULL || target == NULL) {
        return;
    }

    w_mutex_lock(&g_lc_raw_stats_mutex);

    _w_logcollector_state_update_target(g_lc_states_global, fpath, target, dropped);
    _w_logcollector_state_update_target(g_lc_states_interval, fpath, target, dropped);

    w_mutex_unlock(&g_lc_raw_stats_mutex);
}

void w_logcollector_state_update_file(char * fpath, uint64_t bytes) {

    if (fpath == NULL) {
        return;
    }

    w_mutex_lock(&g_lc_raw_stats_mutex);

    _w_logcollector_state_update_file(g_lc_states_global, fpath, bytes);
    _w_logcollector_state_update_file(g_lc_states_interval, fpath, bytes);

    w_mutex_unlock(&g_lc_raw_stats_mutex);
}

void _w_logcollector_state_update_file(lc_states_t * state, char * fpath, uint64_t bytes) {

    lc_state_file_t * data = NULL;

    // Try to get file stats. Create it if not initialized yet,
    if (data = (lc_state_file_t *) OSHash_Get(state->states, fpath), data == NULL) {
        os_calloc(1, sizeof(lc_state_file_t), data);
        os_calloc(1, sizeof(lc_state_target_t *), data->targets);
    }

    if (bytes > 0) {
        data->events++;
        data->bytes += bytes;
    }

    if (OSHash_Update(state->states, fpath, data) != 1) {
        if (OSHash_Add(state->states, fpath, data) != 2) {
            lc_state_target_t ** target = data->targets;
            while (*target != NULL) {
                os_free(*target);
                target++;
            }
            os_free(data->targets);
            os_free(data);
            merror(HUPDATE_ERROR, fpath, LOGCOLLECTOR_STATE_DESCRIPTION);
        }
    }
}

void _w_logcollector_state_update_target(lc_states_t * state, char * fpath, char * target, bool dropped) {

    lc_state_file_t * data = NULL;
    lc_state_target_t ** current_target = NULL;
    int len = 0;

    // Try to get file stats. Create it if not initialized yet,
    if (data = (lc_state_file_t *) OSHash_Get(state->states, fpath), data == NULL) {
        os_calloc(1, sizeof(lc_state_file_t), data);
        os_calloc(1, sizeof(lc_state_target_t *), data->targets);
    }

    // Try to find target
    for (len = 0, current_target = data->targets; *current_target != NULL; len++, current_target++) {
        if (strcmp(target, (*current_target)->name) == 0) {
            break;
        }
    }

    // If target was not found, create it.
    if (*current_target == NULL) {
        os_realloc(data->targets, (len + 2) * sizeof(lc_state_target_t *), data->targets);
        os_calloc(1, sizeof(lc_state_target_t), data->targets[len]);
        data->targets[len + 1] = NULL;
        current_target = &data->targets[len];
        os_strdup(target, (*current_target)->name);
    }

    if (dropped) {
        (*current_target)->drops++;
    }

    if (OSHash_Update(state->states, fpath, data) != 1) {
        if (OSHash_Add(state->states, fpath, data) != 2) {
            lc_state_target_t ** target = data->targets;
            while (*target != NULL) {
                os_free(*target);
                target++;
            }
            os_free(data->targets);
            os_free(data);
            merror(HUPDATE_ERROR, fpath, LOGCOLLECTOR_STATE_DESCRIPTION);
        }
    }
}

void w_logcollector_generate_state() {

    w_mutex_lock(&g_lc_json_stats_mutex);
    w_mutex_lock(&g_lc_raw_stats_mutex);

    cJSON_Delete(g_lc_json_stats);

    g_lc_json_stats = cJSON_CreateObject();
    cJSON * lc_stats_json_global = _w_logcollector_generate_state(g_lc_states_global, false);
    cJSON_AddItemToObject(g_lc_json_stats, "global", lc_stats_json_global);
    cJSON * lc_stats_json_interval = _w_logcollector_generate_state(g_lc_states_interval, true);
    cJSON_AddItemToObject(g_lc_json_stats, "interval", lc_stats_json_interval);

    w_mutex_unlock(&g_lc_raw_stats_mutex);
    w_mutex_unlock(&g_lc_json_stats_mutex);
}

cJSON * w_logcollector_state_get() {

    cJSON * json_state = NULL;

    w_mutex_lock(&g_lc_json_stats_mutex);

    if (g_lc_json_stats != NULL) {
        json_state = cJSON_Duplicate(g_lc_json_stats, true);
    }

    w_mutex_unlock(&g_lc_json_stats_mutex);

    return json_state;
}

cJSON * _w_logcollector_generate_state(lc_states_t * state, bool restart) {

    OSHashNode * hash_node = NULL;
    unsigned int index = 0;

    if (hash_node = OSHash_Begin(state->states, &index), hash_node == NULL) {
        return NULL;
    }

    cJSON * lc_stats_json = cJSON_CreateObject();
    cJSON * lc_stats_files_array = cJSON_CreateArray();

    // Iterate for each file
    while (hash_node) {
        lc_state_file_t * data = hash_node->data;

        // Target logic
        cJSON * lc_stats_targets_array = cJSON_CreateArray();
        lc_state_target_t ** target = data->targets;
        while (*target != NULL) {
            cJSON * lc_stats_target = cJSON_CreateObject();
            cJSON_AddStringToObject(lc_stats_target, "name", (*target)->name);
            cJSON_AddNumberToObject(lc_stats_target, "drops", (*target)->drops);
            cJSON_AddItemToArray(lc_stats_targets_array, lc_stats_target);
            if (restart) {
                (*target)->drops = 0;
            }
            target++;
        }

        // Files
        cJSON * lc_stats_file = cJSON_CreateObject();
        cJSON_AddItemToObject(lc_stats_file, "targets", lc_stats_targets_array);
        cJSON_AddStringToObject(lc_stats_file, "location", hash_node->key);
        cJSON_AddNumberToObject(lc_stats_file, "bytes", data->bytes);
        cJSON_AddNumberToObject(lc_stats_file, "events", data->events);
        if (restart) {
            data->bytes = 0;
            data->events = 0;
        }
        cJSON_AddItemToArray(lc_stats_files_array, lc_stats_file);
        hash_node = OSHash_Next(state->states, &index, hash_node);
    }

    // Convert timestamp to string, removing newline from ctime return
    time_t now = time(NULL);
    char * now_str = NULL;
    char * start_str = NULL;
    os_strdup(ctime(&now), now_str);
    os_strdup(ctime(&state->start), start_str);

    now_str[strlen(now_str) - 1] = '\0';
    start_str[strlen(start_str) - 1] = '\0';

    cJSON_AddStringToObject(lc_stats_json, "start", start_str);
    cJSON_AddStringToObject(lc_stats_json, "end", now_str);
    cJSON_AddItemToObject(lc_stats_json, "files", lc_stats_files_array);

    os_free(now_str);
    os_free(start_str);

    if (restart) {
        state->start = time(NULL);
    }
    return lc_stats_json;
}
