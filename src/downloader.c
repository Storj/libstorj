#include "downloader.h"

static void free_bucket_token(storj_download_state_t *state)
{
    if (state->token) {
        free(state->token);
        state->token = NULL;
    }
}

static void free_exchange_report(storj_exchange_report_t *report)
{
    free(report->data_hash);
    free(report->reporter_id);
    free(report->farmer_id);
    free(report->client_id);
    free(report);
}

static void free_download_state(storj_download_state_t *state)
{
    for (int i = 0; i < state->total_pointers; i++) {
        storj_pointer_t *pointer = &state->pointers[i];

        free(pointer->token);
        free(pointer->shard_hash);
        free(pointer->farmer_id);
        free(pointer->farmer_address);

        free_exchange_report(pointer->report);
    }


    free_bucket_token(state);

    if (state->excluded_farmer_ids) {
        free(state->excluded_farmer_ids);
    }

    if (state->decrypt_key) {
        memset_zero(state->decrypt_key, SHA256_DIGEST_SIZE);
        free(state->decrypt_key);
    }

    if (state->decrypt_ctr) {
        memset_zero(state->decrypt_ctr, AES_BLOCK_SIZE);
        free(state->decrypt_ctr);
    }

    if (state->info) {
        free((char *)state->info->hmac);
        free(state->info);
    }

    free(state->hmac_ctx);
    free(state->pointers);
    free(state);
}

static void request_token(uv_work_t *work)
{
    token_request_download_t *req = work->data;
    storj_download_state_t *state = req->state;

    int path_len = 9 + strlen(req->bucket_id) + 7;
    char *path = calloc(path_len + 1, sizeof(char));
    if (!path) {
        req->error_status = STORJ_MEMORY_ERROR;
        return;
    }

    strcat(path, "/buckets/");
    strcat(path, req->bucket_id);
    strcat(path, "/tokens");

    struct json_object *body = json_object_new_object();
    json_object *op_string = json_object_new_string(req->bucket_op);
    json_object_object_add(body, "operation", op_string);

    int status_code = 0;
    struct json_object *response = NULL;
    int request_status = fetch_json(req->http_options,
                                    req->options,
                                    "POST",
                                    path,
                                    body,
                                    true,
                                    NULL,
                                    &response,
                                    &status_code);

    if (request_status) {
        req->error_status = STORJ_BRIDGE_REQUEST_ERROR;

        state->log->warn(state->env->log_options, state->handle,
                         "Request token error: %i", request_status);

    } else if (status_code == 201) {
        struct json_object *token_value;
        if (!json_object_object_get_ex(response, "token", &token_value)) {
            req->error_status = STORJ_BRIDGE_JSON_ERROR;
        }

        if (!json_object_is_type(token_value, json_type_string)) {
            req->error_status = STORJ_BRIDGE_JSON_ERROR;
        }

        char *token = (char *)json_object_get_string(token_value);
        req->token = strdup(token);

    } else if (status_code == 403 || status_code == 401) {
        req->error_status = STORJ_BRIDGE_AUTH_ERROR;
    } else if (status_code == 404) {
        req->error_status = STORJ_BRIDGE_BUCKET_NOTFOUND_ERROR;
    } else if (status_code == 500) {
        req->error_status = STORJ_BRIDGE_INTERNAL_ERROR;
    } else {
        req->error_status = STORJ_BRIDGE_REQUEST_ERROR;
    }

    req->status_code = status_code;

    if (response) {
        json_object_put(response);
    }
    json_object_put(body);
    free(path);
}

static void after_request_token(uv_work_t *work, int status)
{
    token_request_download_t *req = work->data;

    req->state->pending_work_count--;
    req->state->requesting_token = false;

    if (status != 0) {
        req->state->error_status = STORJ_BRIDGE_TOKEN_ERROR;
    } else if (req->status_code == 201) {
        req->state->token = req->token;
    } else if (req->error_status){
        switch (req->error_status) {
            case STORJ_BRIDGE_REQUEST_ERROR:
            case STORJ_BRIDGE_INTERNAL_ERROR:
                req->state->token_fail_count += 1;
                break;
            default:
                req->state->error_status = req->error_status;
                break;
        }
        if (req->state->token_fail_count >= STORJ_MAX_TOKEN_TRIES) {
            req->state->token_fail_count = 0;
            req->state->error_status = req->error_status;
        }

    } else {
        req->state->error_status = STORJ_BRIDGE_TOKEN_ERROR;
    }

    queue_next_work(req->state);

    free(req);
    free(work);
}

static void queue_request_bucket_token(storj_download_state_t *state)
{
    if (state->requesting_token || state->canceled) {
        return;
    }

    uv_work_t *work = malloc(sizeof(uv_work_t));
    if (!work) {
        state->error_status = STORJ_MEMORY_ERROR;
        return;
    }

    token_request_download_t *req = malloc(sizeof(token_request_download_t));
    if (!req) {
        state->error_status = STORJ_MEMORY_ERROR;
        return;
    }

    req->http_options = state->env->http_options;
    req->options = state->env->bridge_options;
    req->bucket_id = state->bucket_id;
    req->bucket_op = (char *)BUCKET_OP[BUCKET_PULL];
    req->state = state;
    req->status_code = 0;
    req->error_status = 0;

    work->data = req;

    state->pending_work_count++;
    int status = uv_queue_work(state->env->loop, (uv_work_t*) work,
                               request_token, after_request_token);

    if (status) {
        state->error_status = STORJ_QUEUE_ERROR;
        return;
    }

    state->requesting_token = true;

}

static void request_pointers(uv_work_t *work)
{
    json_request_download_t *req = work->data;
    storj_download_state_t *state = req->state;

    int status_code = 0;
    int request_status = fetch_json(req->http_options, req->options, req->method,
                                    req->path, req->body, req->auth, req->token,
                                    &req->response, &status_code);


    if (request_status) {
        state->log->warn(state->env->log_options, state->handle,
                         "Request pointers error: %i", request_status);
    }

    req->status_code = status_code;

    if (!req->response) {
        req->status_code = -1;
    }
}

static void request_replace_pointer(uv_work_t *work)
{
    json_request_replace_pointer_t *req = work->data;
    storj_download_state_t *state = req->state;

    int status_code = 0;

    char query_args[32 + strlen(req->excluded_farmer_ids)];
    snprintf(query_args, 25 + strlen(req->excluded_farmer_ids),
             "?limit=1&skip=%i&exclude=%s",
             req->pointer_index,
             req->excluded_farmer_ids);

    int path_len = 9 + strlen(req->bucket_id) + 7 +
        strlen(req->file_id) + strlen(query_args);
    char *path = calloc(path_len + 1, sizeof(char));
    if (!path) {
        req->error_status = STORJ_MEMORY_ERROR;
        return;
    }

    strcat(path, "/buckets/");
    strcat(path, req->bucket_id);
    strcat(path, "/files/");
    strcat(path, req->file_id);
    strcat(path, query_args);

    int request_status = fetch_json(req->http_options, req->options, "GET",
                                    path, NULL, NULL, req->token,
                                    &req->response, &status_code);

    if (request_status) {
        state->log->warn(state->env->log_options, state->handle,
                         "Request replace pointer error: %i", request_status);
    }


    req->status_code = status_code;

    if (!req->response) {
        req->status_code = -1;
    }

    free(path);

}

static void set_pointer_from_json(storj_download_state_t *state,
                                  storj_pointer_t *p,
                                  struct json_object *json,
                                  bool is_replaced)
{
    if (!json_object_is_type(json, json_type_object)) {
        state->error_status = STORJ_BRIDGE_JSON_ERROR;
        return;
    }

    struct json_object *token_value;
    if (!json_object_object_get_ex(json, "token", &token_value)) {
        state->error_status = STORJ_BRIDGE_JSON_ERROR;
        return;
    }
    char *token = (char *)json_object_get_string(token_value);

    struct json_object *hash_value;
    if (!json_object_object_get_ex(json, "hash", &hash_value)) {
        state->error_status = STORJ_BRIDGE_JSON_ERROR;
        return;
    }
    char *hash = (char *)json_object_get_string(hash_value);

    struct json_object *size_value;
    if (!json_object_object_get_ex(json, "size", &size_value)) {
        state->error_status = STORJ_BRIDGE_JSON_ERROR;
        return;
    }
    uint64_t size = json_object_get_int64(size_value);


    struct json_object *index_value;
    if (!json_object_object_get_ex(json, "index", &index_value)) {
        state->error_status = STORJ_BRIDGE_JSON_ERROR;
        return;
    }
    uint32_t index = json_object_get_int(index_value);

    struct json_object *farmer_value;
    if (!json_object_object_get_ex(json, "farmer", &farmer_value)) {
        state->error_status = STORJ_BRIDGE_JSON_ERROR;
        return;
    }
    if (!json_object_is_type(farmer_value, json_type_object)) {
        state->error_status = STORJ_BRIDGE_JSON_ERROR;
        return;
    }

    struct json_object *address_value;
    if (!json_object_object_get_ex(farmer_value, "address",
                                   &address_value)) {
        state->error_status = STORJ_BRIDGE_JSON_ERROR;
        return;
    }
    char *address = (char *)json_object_get_string(address_value);

    struct json_object *port_value;
    if (!json_object_object_get_ex(farmer_value, "port", &port_value)) {
        state->error_status = STORJ_BRIDGE_JSON_ERROR;
        return;
    }
    uint32_t port = json_object_get_int(port_value);

    struct json_object *farmer_id_value;
    if (!json_object_object_get_ex(farmer_value, "nodeID",
                                   &farmer_id_value)) {
        state->error_status = STORJ_BRIDGE_JSON_ERROR;
        return;
    }
    char *farmer_id = (char *)json_object_get_string(farmer_id_value);

    if (is_replaced) {
        p->replace_count += 1;
    } else {
        p->replace_count = 0;
    }

    // reset the status
    p->status = POINTER_CREATED;

    p->size = size;
    p->downloaded_size = 0;
    p->index = index;
    p->farmer_port = port;

    if (is_replaced) {
        free(p->token);
        free(p->shard_hash);
        free(p->farmer_address);
        free(p->farmer_id);
    }
    p->token = strdup(token);
    p->shard_hash = strdup(hash);
    p->farmer_address = strdup(address);
    p->farmer_id = strdup(farmer_id);

    // setup exchange report values
    if (is_replaced) {
        free_exchange_report(p->report);
    }
    p->report = malloc(
        sizeof(storj_exchange_report_t));

    if (!p->report) {
        state->error_status = STORJ_MEMORY_ERROR;
        return;
    }

    const char *client_id = state->env->bridge_options->user;
    p->report->reporter_id = strdup(client_id);
    p->report->client_id = strdup(client_id);
    p->report->data_hash = strdup(hash);
    p->report->farmer_id = strdup(farmer_id);
    p->report->send_status = 0; // not sent
    p->report->send_count = 0;

    // these values will be changed in after_request_shard
    p->report->start = 0;
    p->report->end = 0;
    p->report->code = STORJ_REPORT_FAILURE;
    p->report->message = STORJ_REPORT_DOWNLOAD_ERROR;

    p->work = NULL;

    if (!state->shard_size) {
        // TODO make sure all except last shard is the same size
        state->shard_size = size;
    };
}

static void append_pointers_to_state(storj_download_state_t *state,
                                     struct json_object *res)
{
    int length = json_object_array_length(res);

    if (length == 0) {
        state->pointers_completed = true;
    } else if (length > 0) {

        int prev_total_pointers = state->total_pointers;
        int total_pointers = state->total_pointers + length;

        if (state->total_pointers > 0) {
            state->pointers = realloc(state->pointers,
                                      total_pointers * sizeof(storj_pointer_t));
        } else {
            state->pointers = malloc(length * sizeof(storj_pointer_t) * 100);
        }
        if (!state->pointers) {
            state->error_status = STORJ_MEMORY_ERROR;
            return;
        }

        state->total_pointers = total_pointers;
        state->total_shards = total_pointers;

        for (int i = 0; i < length; i++) {

            // get the relative index
            int j = i + prev_total_pointers;

            struct json_object *json = json_object_array_get_idx(res, i);

            set_pointer_from_json(state, &state->pointers[j], json, false);
        }
    }

}

static void after_request_pointers(uv_work_t *work, int status)
{
    json_request_download_t *req = work->data;

    req->state->pending_work_count--;
    req->state->requesting_pointers = false;

    free_bucket_token(req->state);

    if (status != 0)  {
        req->state->error_status = STORJ_BRIDGE_POINTER_ERROR;
    } else if (req->status_code != 200) {
        if (req->status_code > 0 && req->status_code < 500) {
            req->state->error_status = STORJ_BRIDGE_POINTER_ERROR;
        } else {
            req->state->pointer_fail_count += 1;
        }

        if (req->state->pointer_fail_count >= STORJ_MAX_POINTER_TRIES) {
            req->state->pointer_fail_count = 0;
            req->state->error_status = STORJ_BRIDGE_POINTER_ERROR;
        }

    } else if (!json_object_is_type(req->response, json_type_array)) {
        req->state->error_status = STORJ_BRIDGE_JSON_ERROR;
    } else {
        append_pointers_to_state(req->state, req->response);
    }

    queue_next_work(req->state);

    json_object_put(req->response);
    free(req->path);
    free(req);
    free(work);
}

static void after_request_replace_pointer(uv_work_t *work, int status)
{
    json_request_replace_pointer_t *req = work->data;

    req->state->pending_work_count--;
    req->state->requesting_pointers = false;

    free_bucket_token(req->state);

    if (status != 0) {
        req->state->error_status = STORJ_BRIDGE_REPOINTER_ERROR;
    } else if (req->error_status) {
        req->state->error_status = req->error_status;
    } else if (req->status_code != 200) {
        req->state->error_status = STORJ_BRIDGE_REPOINTER_ERROR;

        if (req->status_code > 0 && req->status_code < 500) {
            req->state->error_status = STORJ_BRIDGE_POINTER_ERROR;
        } else {
            req->state->pointer_fail_count += 1;
        }

        if (req->state->pointer_fail_count >= STORJ_MAX_POINTER_TRIES) {
            req->state->pointer_fail_count = 0;
            req->state->error_status = STORJ_BRIDGE_POINTER_ERROR;
        }

    } else if (!json_object_is_type(req->response, json_type_array)) {
        req->state->error_status = STORJ_BRIDGE_JSON_ERROR;
    } else {
        struct json_object *json = json_object_array_get_idx(req->response, 0);
        // TODO check json

        // TODO check that the index of pointer matches what is expected
        // TODO check that the shard hash is the same

        set_pointer_from_json(req->state,
                              &req->state->pointers[req->pointer_index],
                              json,
                              true);
    }

    queue_next_work(req->state);

    json_object_put(req->response);
    free(work->data);
    free(work);

}

static void queue_request_pointers(storj_download_state_t *state)
{
    if (state->requesting_pointers || state->canceled) {
        return;
    }

    // queue request to replace pointer if any pointers have failure
    for (int i = 0; i < state->total_pointers; i++) {

        storj_pointer_t *pointer = &state->pointers[i];

        if (pointer->replace_count >= STORJ_DEFAULT_MIRRORS) {
            state->error_status = STORJ_FARMER_EXHAUSTED_ERROR;
            return;
        }

        if (pointer->status == POINTER_ERROR_REPORTED) {

            // exclude this farmer id from future requests
            if (!state->excluded_farmer_ids) {
                state->excluded_farmer_ids = calloc(41, sizeof(char));
                if (!state->excluded_farmer_ids) {
                    state->error_status = STORJ_MEMORY_ERROR;
                    return;
                }
                strcat(state->excluded_farmer_ids, pointer->report->farmer_id);
            } else {
                state->excluded_farmer_ids =
                    realloc(state->excluded_farmer_ids,
                            strlen(state->excluded_farmer_ids) + 41);
                if (!state->excluded_farmer_ids) {
                    state->error_status = STORJ_MEMORY_ERROR;
                    return;
                }
                strcat(state->excluded_farmer_ids, ",");
                strcat(state->excluded_farmer_ids, pointer->report->farmer_id);
            }

            json_request_replace_pointer_t *req =
                malloc(sizeof(json_request_replace_pointer_t));
            if (!req) {
                state->error_status = STORJ_MEMORY_ERROR;
                return;
            }

            req->pointer_index = i;

            req->http_options = state->env->http_options;
            req->options = state->env->bridge_options;
            req->token = state->token;
            req->bucket_id = state->bucket_id;
            req->file_id = state->file_id;
            req->state = state;
            req->excluded_farmer_ids = state->excluded_farmer_ids;
            req->error_status = 0;
            req->response = NULL;
            req->status_code = 0;

            uv_work_t *work = malloc(sizeof(uv_work_t));
            if (!work) {
                state->error_status = STORJ_MEMORY_ERROR;
                return;
            }
            work->data = req;

            state->log->info(state->env->log_options,
                             state->handle,
                             "Requesting replacement pointer at index: %i",
                             req->pointer_index);

            state->pending_work_count++;
            int status = uv_queue_work(state->env->loop,
                                       (uv_work_t*) work,
                                       request_replace_pointer,
                                       after_request_replace_pointer);

            if (status) {
                state->error_status = STORJ_QUEUE_ERROR;
                return;
            }

            pointer->status = POINTER_BEING_REPLACED;

            // we're done until the next pass
            state->requesting_pointers = true;
            return;
        }

    }

    // only request the next set of pointers if we're not finished
    if (state->pointers_completed) {
        return;
    }

    json_request_download_t *req = malloc(sizeof(json_request_download_t));
    if (!req) {
        state->error_status = STORJ_MEMORY_ERROR;
        return;
    }

    char query_args[32];
    snprintf(query_args, 20, "?limit=6&skip=%i", state->total_pointers);

    int path_len = 9 + strlen(state->bucket_id) + 7 +
        strlen(state->file_id) + strlen(query_args);

    char *path = calloc(path_len + 1, sizeof(char));
    if (!path) {
        state->error_status = STORJ_MEMORY_ERROR;
        return;
    }
    strcat(path, "/buckets/");
    strcat(path, state->bucket_id);
    strcat(path, "/files/");
    strcat(path, state->file_id);
    strcat(path, query_args);

    req->http_options = state->env->http_options;
    req->options = state->env->bridge_options;
    req->method = "GET";
    req->path = path;
    req->body = NULL;
    req->auth = true;
    req->token = state->token;

    req->state = state;

    uv_work_t *work = malloc(sizeof(uv_work_t));
    if (!work) {
        state->error_status = STORJ_MEMORY_ERROR;
        return;
    }
    work->data = req;

    state->log->info(state->env->log_options,
                     state->handle,
                     "Requesting next set of pointers, total pointers: %i",
                     state->total_pointers);

    state->pending_work_count++;
    int status = uv_queue_work(state->env->loop, (uv_work_t*) work,
                               request_pointers, after_request_pointers);

    if (status) {
        state->error_status = STORJ_QUEUE_ERROR;
        return;
    }

    state->requesting_pointers = true;
}

static void request_shard(uv_work_t *work)
{
    shard_request_download_t *req = work->data;

    int status_code;

    req->start = get_time_milliseconds();

    int error_status = fetch_shard(req->http_options, req->farmer_id,
                                   req->farmer_proto, req->farmer_host,
                                   req->farmer_port, req->shard_hash,
                                   req->shard_total_bytes, req->shard_data,
                                   req->token, &status_code,
                                   &req->progress_handle,
                                   req->canceled);

    req->end = get_time_milliseconds();

    if (error_status) {
        req->error_status = error_status;
    } else if (status_code != 200) {
        switch(status_code) {
            case 401:
            case 403:
                req->error_status = STORJ_FARMER_AUTH_ERROR;
                break;
            case 504:
                req->error_status = STORJ_FARMER_TIMEOUT_ERROR;
                break;
            default:
                req->error_status = STORJ_FARMER_REQUEST_ERROR;
        }
    } else {
        // Decrypt the shard
        if (req->decrypt_key && req->decrypt_ctr) {
            struct aes256_ctx *ctx = malloc(sizeof(struct aes256_ctx));
            if (!ctx) {
                req->error_status = STORJ_MEMORY_ERROR;
                return;
            }

            aes256_set_encrypt_key(ctx, req->decrypt_key);
            ctr_crypt(ctx, (nettle_cipher_func *)aes256_encrypt,
                      AES_BLOCK_SIZE, req->decrypt_ctr,
                      req->shard_total_bytes,
                      (uint8_t *)req->shard_data,
                      (uint8_t *)req->shard_data);

            free(ctx);
        }

        req->error_status = 0;
    }
}

static void free_request_shard_work(uv_handle_t *progress_handle)
{
    uv_work_t *work = progress_handle->data;
    shard_request_download_t *req = work->data;

    memset_zero(req->decrypt_key, SHA256_DIGEST_SIZE);
    free(req->decrypt_key);

    memset_zero(req->decrypt_ctr, AES_BLOCK_SIZE);
    free(req->decrypt_ctr);

    free(req);
    free(work);
}

static void after_request_shard(uv_work_t *work, int status)
{
    shard_request_download_t *req = work->data;

    req->state->pending_work_count--;
    req->state->resolving_shards -= 1;

    uv_handle_t *progress_handle = (uv_handle_t *) &req->progress_handle;

    // free the download progress
    free(progress_handle->data);

    // assign work so that we can free after progress_handle is closed
    progress_handle->data = work;

    // update the pointer status
    storj_pointer_t *pointer = &req->state->pointers[req->pointer_index];

    pointer->report->start = req->start;
    pointer->report->end = req->end;

    if (req->error_status) {

        req->state->log->warn(req->state->env->log_options,
                              req->state->handle,
                              "Error downloading shard: %s, reason: %s",
                              req->shard_hash,
                              storj_strerror(req->error_status));

        pointer->status = POINTER_ERROR;

        switch(req->error_status) {
            case STORJ_FARMER_INTEGRITY_ERROR:
                pointer->report->code = STORJ_REPORT_FAILURE;
                pointer->report->message = STORJ_REPORT_FAILED_INTEGRITY;
            default:
                pointer->report->code = STORJ_REPORT_FAILURE;
                pointer->report->message = STORJ_REPORT_DOWNLOAD_ERROR;
        }

        // TODO don't send error on canceled download

        free(req->shard_data);

    } else {

        req->state->log->info(req->state->env->log_options,
                              req->state->handle,
                              "Finished downloading shard: %s",
                              req->shard_hash);

        pointer->report->code = STORJ_REPORT_SUCCESS;
        pointer->report->message = STORJ_REPORT_SHARD_DOWNLOADED;
        pointer->status = POINTER_DOWNLOADED;
        pointer->shard_data = req->shard_data;
    }

    queue_next_work(req->state);

    // close the async progress handle
    uv_close(progress_handle, free_request_shard_work);
}

static void progress_request_shard(uv_async_t* async)
{

    shard_download_progress_t *progress = async->data;

    storj_download_state_t *state = progress->state;

    state->pointers[progress->pointer_index].downloaded_size = progress->bytes;

    uint64_t downloaded_bytes = 0;
    uint64_t total_bytes = 0;

    for (int i = 0; i < state->total_pointers; i++) {

        storj_pointer_t *pointer = &state->pointers[i];

        downloaded_bytes += pointer->downloaded_size;
        total_bytes += pointer->size;
    }

    double total_progress = (double)downloaded_bytes / (double)total_bytes;

    state->progress_cb(total_progress,
                       downloaded_bytes,
                       total_bytes,
                       state->handle);
}

static void queue_request_shards(storj_download_state_t *state)
{
    if (state->canceled) {
        return;
    }

    int i = 0;

    while (state->resolving_shards < STORJ_DOWNLOAD_CONCURRENCY &&
           i < state->total_pointers) {

        storj_pointer_t *pointer = &state->pointers[i];

        if (pointer->status == POINTER_CREATED) {
            shard_request_download_t *req = malloc(sizeof(shard_request_download_t));
            if (!req) {
                state->error_status = STORJ_MEMORY_ERROR;
                return;
            }

            req->http_options = state->env->http_options;
            req->farmer_id = pointer->farmer_id;
            req->farmer_proto = "http";
            req->farmer_host = pointer->farmer_address;
            req->farmer_port = pointer->farmer_port;
            req->shard_hash = pointer->shard_hash;
            req->shard_total_bytes = pointer->size;
            req->byte_position = state->shard_size * i;
            req->token = pointer->token;

            // TODO assert max bytes for shard
            req->shard_data = calloc(pointer->size, sizeof(char));
            if (!req->shard_data) {
                state->error_status = STORJ_MEMORY_ERROR;
                return;
            }

            if (state->decrypt_key && state->decrypt_ctr) {
                req->decrypt_key = calloc(SHA256_DIGEST_SIZE, sizeof(uint8_t));
                if (!req->decrypt_key) {
                    state->error_status = STORJ_MEMORY_ERROR;
                    return;
                }
                req->decrypt_ctr = calloc(AES_BLOCK_SIZE, sizeof(uint8_t));
                if (!req->decrypt_ctr) {
                    state->error_status = STORJ_MEMORY_ERROR;
                    return;
                }
                memcpy(req->decrypt_key, state->decrypt_key, SHA256_DIGEST_SIZE);
                memcpy(req->decrypt_ctr, state->decrypt_ctr, AES_BLOCK_SIZE);

                increment_ctr_aes_iv(req->decrypt_ctr, req->byte_position);
            } else {
                req->decrypt_key = NULL;
                req->decrypt_ctr = NULL;
            }

            req->pointer_index = pointer->index;

            req->state = state;
            req->canceled = &state->canceled;

            uv_work_t *work = malloc(sizeof(uv_work_t));
            if (!work) {
                state->error_status = STORJ_MEMORY_ERROR;
                return;
            }

            work->data = req;

            state->resolving_shards += 1;
            pointer->status = POINTER_BEING_DOWNLOADED;
            pointer->work = work;

            state->log->info(state->env->log_options,
                             state->handle,
                             "Queue request shard: %s",
                             req->shard_hash);

            // setup download progress reporting
            shard_download_progress_t *progress =
                malloc(sizeof(shard_download_progress_t));
            if (!progress) {
                state->error_status = STORJ_MEMORY_ERROR;
                return;
            }

            progress->pointer_index = pointer->index;
            progress->bytes = 0;
            progress->state = state;

            req->progress_handle.data = progress;

            uv_async_init(state->env->loop, &req->progress_handle,
                          progress_request_shard);

            // queue download
            state->pending_work_count++;
            int status = uv_queue_work(state->env->loop, (uv_work_t*) work,
                                       request_shard, after_request_shard);
            if (status) {
                state->error_status = STORJ_QUEUE_ERROR;
                return;
            }
        }

        i++;
    }
}

static void write_shard(uv_work_t *work)
{
    shard_request_write_t *req = work->data;
    req->error_status = 0;

    // multiple write shards will not happen at the same
    // time so it should be safe here to use hmac_ctx on state
    // within a worker thread
    hmac_sha512_update(req->state->hmac_ctx,
                       req->shard_total_bytes,
                       req->shard_data);

    if (req->shard_total_bytes != pwrite(fileno(req->destination),
                                         req->shard_data,
                                         req->shard_total_bytes,
                                         req->pointer_index * req->state->shard_size)) {

        req->error_status = STORJ_FILE_WRITE_ERROR;
    }
}

static void after_write_shard(uv_work_t *work, int status)
{
    shard_request_write_t *req = work->data;

    req->state->pending_work_count--;
    req->state->writing = false;

    if (status != 0) {
        req->state->error_status = STORJ_FILE_WRITE_ERROR;
    } else if (req->error_status) {
        req->state->error_status = STORJ_FILE_WRITE_ERROR;
    } else {
        req->state->pointers[req->pointer_index].status = POINTER_WRITTEN;
        req->state->completed_shards += 1;
    }

    free(req->shard_data);

    queue_next_work(req->state);

    free(work->data);
    free(work);
}

static void queue_write_next_shard(storj_download_state_t *state)
{
    if (state->canceled) {
        return;
    }

    int i = 0;

    while (!state->writing && i < state->total_pointers) {
        storj_pointer_t *pointer = &state->pointers[i];

        if (pointer->status < POINTER_DOWNLOADED) {
            break;
        }

        if (pointer->status == POINTER_DOWNLOADED) {
            uv_work_t *work = malloc(sizeof(uv_work_t));
            if (!work) {
                state->error_status = STORJ_MEMORY_ERROR;
                return;
            }

            shard_request_write_t *req = malloc(sizeof(shard_request_write_t));
            if (!req) {
                state->error_status = STORJ_MEMORY_ERROR;
                return;
            }

            req->shard_data = pointer->shard_data;
            req->shard_total_bytes = pointer->size;
            req->destination = state->destination;
            req->pointer_index = pointer->index;
            req->state = state;

            work->data = req;

            state->writing = true;
            pointer->status = POINTER_BEING_WRITTEN;

            state->pending_work_count++;
            int status = uv_queue_work(state->env->loop, (uv_work_t*) work,
                                       write_shard, after_write_shard);
            if (status) {
                state->error_status = STORJ_QUEUE_ERROR;
            }
            break;
        }

        i++;

    }
}

static void send_exchange_report(uv_work_t *work)
{
    shard_send_report_t *req = work->data;
    storj_download_state_t *state = req->state;

    struct json_object *body = json_object_new_object();

    json_object_object_add(body, "dataHash",
                           json_object_new_string(req->report->data_hash));

    json_object_object_add(body, "reporterId",
                           json_object_new_string(req->report->reporter_id));

    json_object_object_add(body, "farmerId",
                           json_object_new_string(req->report->farmer_id));

    json_object_object_add(body, "clientId",
                           json_object_new_string(req->report->client_id));

    json_object_object_add(body, "exchangeStart",
                           json_object_new_int64(req->report->start));

    json_object_object_add(body, "exchangeEnd",
                           json_object_new_int64(req->report->end));

    json_object_object_add(body, "exchangeResultCode",
                           json_object_new_int(req->report->code));

    json_object_object_add(body, "exchangeResultMessage",
                           json_object_new_string(req->report->message));

    int status_code = 0;

    // there should be an empty object in response
    struct json_object *response = NULL;
    int request_status = fetch_json(req->http_options,
                                    req->options, "POST",
                                    "/reports/exchanges", body,
                                    NULL, NULL, &response, &status_code);


    if (request_status) {
        state->log->warn(state->env->log_options, state->handle,
                         "Send exchange report error: %i", request_status);
    }

    req->status_code = status_code;

    // free all memory for body and response
    if (response) {
        json_object_put(response);
    }
    json_object_put(body);
}

static void after_send_exchange_report(uv_work_t *work, int status)
{
    shard_send_report_t *req = work->data;

    req->state->pending_work_count--;

    // set status so that this pointer can be replaced
    if (req->report->send_count >= STORJ_MAX_REPORT_TRIES ||
        req->status_code == 201) {

        storj_pointer_t *pointer = &req->state->pointers[req->pointer_index];

        if (pointer->status == POINTER_ERROR) {
            pointer->status = POINTER_ERROR_REPORTED;
        }
    }

    if (req->status_code == 201) {
        // set the status so that this pointer can be replaced
        req->report->send_status = 2; // report has been sent
    } else {
        req->report->send_status = 0; // reset report back to unsent
    }

    queue_next_work(req->state);

    free(work->data);
    free(work);

}

static void queue_send_exchange_reports(storj_download_state_t *state)
{

    for (int i = 0; i < state->total_pointers; i++) {

        storj_pointer_t *pointer = &state->pointers[i];

        if (pointer->report->send_status < 1 &&
            pointer->report->send_count < STORJ_MAX_REPORT_TRIES &&
            pointer->report->start > 0 &&
            pointer->report->end > 0) {

            uv_work_t *work = malloc(sizeof(uv_work_t));
            if (!work) {
                state->error_status = STORJ_MEMORY_ERROR;
                return;
            }

            shard_send_report_t *req = malloc(sizeof(shard_send_report_t));
            if (!req) {
                state->error_status = STORJ_MEMORY_ERROR;
                return;
            }

            req->http_options = state->env->http_options;
            req->options = state->env->bridge_options;
            req->status_code = 0;
            req->report = pointer->report;
            req->report->send_status = 1; // being reported
            req->report->send_count += 1;
            req->state = state;
            req->pointer_index = i;

            work->data = req;

            state->pending_work_count++;
            int status = uv_queue_work(state->env->loop, (uv_work_t*) work,
                                       send_exchange_report,
                                       after_send_exchange_report);
            if (status) {
                state->error_status = STORJ_QUEUE_ERROR;
                return;
            }
        }
    }
}

static void after_request_info(uv_work_t *work, int status)
{
    file_info_request_t *req = work->data;

    req->state->pending_work_count--;
    req->state->requesting_info = false;

    if (status != 0) {
        req->state->error_status = STORJ_BRIDGE_FILEINFO_ERROR;
    } else if (req->status_code == 200 || req->status_code == 304) {
        req->state->info = req->info;
    } else if (req->error_status) {
        switch(req->error_status) {
            case STORJ_BRIDGE_REQUEST_ERROR:
            case STORJ_BRIDGE_INTERNAL_ERROR:
                req->state->info_fail_count += 1;
                break;
            default:
                req->state->error_status = req->error_status;
                break;
        }
        if (req->state->info_fail_count >= STORJ_MAX_INFO_TRIES) {
            req->state->info_fail_count = 0;
            req->state->error_status = req->error_status;
        }
    } else {
        req->state->error_status = STORJ_BRIDGE_FILEINFO_ERROR;
    }

    queue_next_work(req->state);

    free(req);
    free(work);

}

static void request_info(uv_work_t *work)
{
    file_info_request_t *req = work->data;
    storj_download_state_t *state = req->state;

    int path_len = 9 + strlen(req->bucket_id) + 7 + strlen(req->file_id) + 5;
    char *path = calloc(path_len + 1, sizeof(char));
    if (!path) {
        req->error_status = STORJ_MEMORY_ERROR;
        return;
    }

    strcat(path, "/buckets/");
    strcat(path, req->bucket_id);
    strcat(path, "/files/");
    strcat(path, req->file_id);
    strcat(path, "/info");

    int status_code = 0;
    struct json_object *response = NULL;
    int request_status = fetch_json(req->http_options,
                                    req->options,
                                    "GET",
                                    path,
                                    NULL,
                                    true,
                                    NULL,
                                    &response,
                                    &status_code);

    req->status_code = status_code;

    if (request_status) {
        req->error_status = STORJ_BRIDGE_REQUEST_ERROR;
        state->log->warn(state->env->log_options, state->handle,
                         "Request file info error: %i", request_status);

    } else if (status_code == 200 || status_code == 304) {
        struct json_object *hmac_obj;
        if (!json_object_object_get_ex(response, "hmac", &hmac_obj)) {
            state->log->warn(state->env->log_options, state->handle,
                             "hmac missing from response");
            goto clean_up;
        }
        if (!json_object_is_type(hmac_obj, json_type_object)) {
            state->log->warn(state->env->log_options, state->handle,
                             "hmac not an object");
            goto clean_up;
        }

        // check the type of hmac
        struct json_object *hmac_type;
        if (!json_object_object_get_ex(hmac_obj, "type", &hmac_type)) {
            state->log->warn(state->env->log_options, state->handle,
                             "hmac.type missing from response");
            goto clean_up;
        }
        if (!json_object_is_type(hmac_type, json_type_string)) {
            state->log->warn(state->env->log_options, state->handle,
                             "hmac.type not a string");
            goto clean_up;
        }
        char *hmac_type_str = (char *)json_object_get_string(hmac_type);
        if (0 != strcmp(hmac_type_str, "sha512")) {
            state->log->warn(state->env->log_options, state->handle,
                             "hmac.type is unknown");
            goto clean_up;
        }

        // get the hmac vlaue
        struct json_object *hmac_value;
        if (!json_object_object_get_ex(hmac_obj, "value", &hmac_value)) {
            state->log->warn(state->env->log_options, state->handle,
                             "hmac.value missing from response");
            goto clean_up;
        }
        if (!json_object_is_type(hmac_value, json_type_string)) {
            state->log->warn(state->env->log_options, state->handle,
                             "hmac.value not a string");
            goto clean_up;
        }
        char *hmac = (char *)json_object_get_string(hmac_value);
        req->info = malloc(sizeof(storj_file_meta_t));
        req->info->hmac = strdup(hmac);

        // TODO set these values, however the are currently
        // not used within the downloader
        req->info->filename = NULL;
        req->info->mimetype = NULL;
        req->info->size = 0;
        req->info->id = NULL;
        req->info->decrypted = false;

    } else if (status_code == 403 || status_code == 401) {
        req->error_status = STORJ_BRIDGE_AUTH_ERROR;
    } else if (status_code == 404) {
        req->error_status = STORJ_BRIDGE_FILE_NOTFOUND_ERROR;
    } else if (status_code == 500) {
        req->error_status = STORJ_BRIDGE_INTERNAL_ERROR;
    } else {
        req->error_status = STORJ_BRIDGE_REQUEST_ERROR;
    }

clean_up:
    if (response) {
        json_object_put(response);
    }
    free(path);
}

static void queue_request_info(storj_download_state_t *state)
{
    if (state->requesting_info || state->canceled) {
        return;
    }

    uv_work_t *work = malloc(sizeof(uv_work_t));
    if (!work) {
        state->error_status = STORJ_MEMORY_ERROR;
        return;
    }

    state->requesting_info = true;

    file_info_request_t *req = malloc(sizeof(file_info_request_t));
    req->http_options = state->env->http_options;
    req->options = state->env->bridge_options;
    req->status_code = 0;
    req->bucket_id = state->bucket_id;
    req->file_id = state->file_id;
    req->error_status = 0;
    req->info = NULL;
    req->state = state;

    work->data = req;

    state->pending_work_count++;
    int status = uv_queue_work(state->env->loop, (uv_work_t*) work,
                               request_info,
                               after_request_info);
    if (status) {
        state->error_status = STORJ_QUEUE_ERROR;
        return;
    }

}

static void queue_next_work(storj_download_state_t *state)
{
    // report any errors
    if (state->error_status != 0) {
        if (!state->finished && state->pending_work_count == 0) {

            state->finished = true;
            state->finished_cb(state->error_status,
                               state->destination,
                               state->handle);

            free_download_state(state);
        }

        return;
    }

    queue_write_next_shard(state);

    // report download complete
    if (state->pointers_completed &&
        state->completed_shards == state->total_shards) {

        if (!state->finished && state->pending_work_count == 0) {

            uint8_t hmac[SHA512_DIGEST_SIZE];
            memset_zero(hmac, SHA512_DIGEST_SIZE);
            hmac_sha512_digest(state->hmac_ctx, SHA512_DIGEST_SIZE, hmac);

            char *hmac_str = hex2str(SHA512_DIGEST_SIZE, hmac);
            if (!hmac_str) {
                state->error_status = STORJ_MEMORY_ERROR;
                return;
            }

            if (state->info && state->info->hmac) {
                if (0 != strcmp(state->info->hmac, hmac_str)) {
                    state->error_status = STORJ_FILE_DECRYPTION_ERROR;
                }
            } else {
                state->log->warn(state->env->log_options,
                                 state->handle,
                                 "Unable to verify decryption integrity" \
                                 ", missing hmac from file info.");
            }

            state->finished = true;
            state->finished_cb(state->error_status, state->destination, state->handle);

            free(hmac_str);
            free_download_state(state);
        }

        return;
    }

    if (!state->token) {
        queue_request_bucket_token(state);
    }

    if (state->token) {
        queue_request_pointers(state);
    }

    if (!state->info) {
        queue_request_info(state);
    }

    queue_request_shards(state);

    // send back download status reports to the bridge
    queue_send_exchange_reports(state);
}

int storj_bridge_resolve_file_cancel(storj_download_state_t *state)
{
    if (state->canceled) {
        return 0;
    }

    state->canceled = true;
    state->error_status = STORJ_TRANSFER_CANCELED;

    // loop over all pointers, and cancel any that are queued to be downloaded
    // any downloads that are in-progress will monitor the state->canceled
    // status and exit when set to true
    for (int i = 0; i < state->total_pointers; i++) {
        storj_pointer_t *pointer = &state->pointers[i];
        if (pointer->status == POINTER_BEING_DOWNLOADED) {
            uv_cancel((uv_req_t *)pointer->work);
        } else if (pointer->status == POINTER_DOWNLOADED) {
            free(pointer->shard_data);
        }
    }

    return 0;
}

int storj_bridge_resolve_file(storj_env_t *env,
                              storj_download_state_t *state,
                              const char *bucket_id,
                              const char *file_id,
                              FILE *destination,
                              void *handle,
                              storj_progress_cb progress_cb,
                              storj_finished_download_cb finished_cb)
{

    // setup download state
    state->total_bytes = 0;
    state->info = NULL;
    state->requesting_info = false;
    state->info_fail_count = 0;
    state->env = env;
    state->file_id = file_id;
    state->bucket_id = bucket_id;
    state->destination = destination;
    state->hmac_ctx = malloc(sizeof(struct hmac_sha512_ctx));
    state->progress_cb = progress_cb;
    state->finished_cb = finished_cb;
    state->finished = false;
    state->total_shards = 0;
    state->completed_shards = 0;
    state->resolving_shards = 0;
    state->total_pointers = 0;
    state->pointers = NULL;
    state->pointers_completed = false;
    state->pointer_fail_count = 0;
    state->requesting_pointers = false;
    state->error_status = STORJ_TRANSFER_OK;
    state->writing = false;
    state->token = NULL;
    state->requesting_token = false;
    state->token_fail_count = 0;
    state->shard_size = 0;
    state->excluded_farmer_ids = NULL;
    state->pending_work_count = 0;
    state->canceled = false;
    state->log = env->log;
    state->handle = handle;

    // determine the decryption key
    if (!env->encrypt_options || !env->encrypt_options->mnemonic) {
        state->decrypt_key = NULL;
        state->decrypt_ctr = NULL;
    } else {
        char *file_key = calloc(DETERMINISTIC_KEY_SIZE + 1, sizeof(char));
        if (!file_key) {
            return STORJ_MEMORY_ERROR;
        }

        if (generate_file_key(env->encrypt_options->mnemonic, bucket_id,
                              file_id, &file_key)) {
            return STORJ_MEMORY_ERROR;
        }
        file_key[DETERMINISTIC_KEY_SIZE] = '\0';

        uint8_t *decrypt_key = calloc(SHA256_DIGEST_SIZE + 1, sizeof(uint8_t));
        if (!decrypt_key) {
            return STORJ_MEMORY_ERROR;
        }

        sha256_of_str((uint8_t *)file_key, DETERMINISTIC_KEY_SIZE, decrypt_key);
        decrypt_key[SHA256_DIGEST_SIZE] = '\0';

        memset_zero(file_key, DETERMINISTIC_KEY_SIZE + 1);
        free(file_key);

        state->decrypt_key = decrypt_key;

        uint8_t *file_id_hash = calloc(RIPEMD160_DIGEST_SIZE + 1, sizeof(uint8_t));
        if (!file_id_hash) {
            return STORJ_MEMORY_ERROR;
        }
        ripemd160_of_str((uint8_t *)file_id, strlen(file_id), file_id_hash);
        file_id_hash[RIPEMD160_DIGEST_SIZE] = '\0';

        uint8_t *decrypt_ctr = calloc(AES_BLOCK_SIZE, sizeof(uint8_t));
        if (!decrypt_ctr) {
            return STORJ_MEMORY_ERROR;
        }
        memcpy(decrypt_ctr, file_id_hash, AES_BLOCK_SIZE);

        free(file_id_hash);

        state->decrypt_ctr = decrypt_ctr;
    };

    // initialize the hmac with the decrypt key
    hmac_sha512_set_key(state->hmac_ctx, SHA256_DIGEST_SIZE, state->decrypt_key);

    // start download
    queue_next_work(state);

    return state->error_status;
}
