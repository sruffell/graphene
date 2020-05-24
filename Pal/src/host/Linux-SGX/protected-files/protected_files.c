/* Copyright (C) 2019-2020 Invisible Things Lab
                           Rafal Wojdyla <omeg@invisiblethingslab.com>

   This file is part of Graphene Library OS.

   Graphene Library OS is free software: you can redistribute it and/or
   modify it under the terms of the GNU Lesser General Public License
   as published by the Free Software Foundation, either version 3 of the
   License, or (at your option) any later version.

   Graphene Library OS is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU Lesser General Public License for more details.

   You should have received a copy of the GNU Lesser General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>.  */

/*
 * Copyright (C) 2011-2019 Intel Corporation. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 *   * Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in
 *     the documentation and/or other materials provided with the
 *     distribution.
 *   * Neither the name of Intel Corporation nor the names of its
 *     contributors may be used to endorse or promote products derived
 *     from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */

#include "api.h"
#include "protected_files_internal.h"

#ifdef IN_PAL
char* strncpy(char* dest, const char* src, size_t n) {
    size_t i;
    for (i = 0; i < n && src[i] != '\0'; i++)
        dest[i] = src[i];
    for ( ; i < n; i++)
        dest[i] = '\0';

    return dest;
}
#else
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#endif

/* Function for scrubbing sensitive memory buffers.
 * memset() can be optimized away and memset_s() is not available in PAL.
 * FIXME: this implementation is inefficient (and used in perf-critical functions),
 * replace with a better one. */
 static void erase_memory(void *buffer, size_t size) {
    volatile unsigned char *p = buffer;
    while (size--)
        *p++ = 0;
}

/* Host callbacks */
static pf_read_f     cb_read     = NULL;
static pf_write_f    cb_write    = NULL;
static pf_truncate_f cb_truncate = NULL;
static pf_open_f     cb_open     = NULL;
static pf_close_f    cb_close    = NULL;
static pf_delete_f   cb_delete   = NULL;
static pf_debug_f    cb_debug    = NULL;

static pf_aes_gcm_encrypt_f cb_aes_gcm_encrypt = NULL;
static pf_aes_gcm_decrypt_f cb_aes_gcm_decrypt = NULL;
static pf_random_f          cb_random          = NULL;

#ifdef DEBUG
#define PF_DEBUG_PRINT_SIZE_MAX 4096

/* Debug print without function name prefix. Implicit param: pf (context pointer). */
#define __DEBUG_PF(format, ...) \
    do { \
        if (cb_debug) { \
            snprintf(pf->debug_buffer, PF_DEBUG_PRINT_SIZE_MAX, format, ##__VA_ARGS__); \
            cb_debug(pf->debug_buffer); \
        } \
    } while(0)

/* Debug print with function name prefix. Implicit param: pf (context pointer). */
#define DEBUG_PF(format, ...) \
    do { \
        if (cb_debug) { \
            snprintf(pf->debug_buffer, PF_DEBUG_PRINT_SIZE_MAX, "%s: " format, __FUNCTION__, \
                     ##__VA_ARGS__); \
            cb_debug(pf->debug_buffer); \
        } \
    } while(0)

#else /* DEBUG */
#define DEBUG_PF(...)
#define __DEBUG_PF(...)
#endif /* DEBUG */

static pf_iv_t g_empty_iv = {0};
static bool g_initialized = false;

#define MASTER_KEY_NAME       "SGX-PROTECTED-FS-MASTER-KEY"
#define RANDOM_KEY_NAME       "SGX-PROTECTED-FS-RANDOM-KEY"
#define METADATA_KEY_NAME     "SGX-PROTECTED-FS-METADATA-KEY"
#define MAX_LABEL_SIZE        64
#define MAX_MASTER_KEY_USAGES 65536

static_assert(sizeof(MASTER_KEY_NAME) <= MAX_LABEL_SIZE, "label too long");
static_assert(sizeof(RANDOM_KEY_NAME) <= MAX_LABEL_SIZE, "label too long");
static_assert(sizeof(METADATA_KEY_NAME) <= MAX_LABEL_SIZE, "label too long");

#pragma pack(push, 1)
typedef struct {
    uint32_t index;
    char label[MAX_LABEL_SIZE]; // must be NULL terminated
    uint64_t node_number; // context 1
    pf_keyid_t nonce;
    uint32_t output_len; // in bits
} kdf_input_t;
#pragma pack(pop)

// The key derivation functions follow recommendations from NIST Special Publication 800-108:
// Recommendation for Key Derivation Using Pseudorandom Functions
// https://nvlpubs.nist.gov/nistpubs/Legacy/SP/nistspecialpublication800-108.pdf

// derive a new random key from another key
static bool ipf_generate_secure_blob(pf_context_t* pf, pf_key_t* key, const char* label,
                                     uint64_t physical_node_number, pf_key_t* output) {
    kdf_input_t buf = {0};

    DEBUG_PF("label: %s, node: %lu\n", label, physical_node_number);
    size_t len = strlen(label);
    if (len > MAX_LABEL_SIZE - 1) {
        pf->last_error = PF_STATUS_INVALID_PARAMETER;
        return false;
    }

    buf.index = 1;
    strncpy(buf.label, label, len);
    buf.node_number = physical_node_number;

    pf_status_t status = cb_random((uint8_t*)&buf.nonce, sizeof(buf.nonce));
    if (PF_FAILURE(status)) {
        pf->last_error = status;
        return false;
    }

    // length of output (128 bits)
    buf.output_len = 0x80;

    status = cb_aes_gcm_encrypt(key, &g_empty_iv, &buf, sizeof(buf), NULL, 0, NULL, output);
    if (PF_FAILURE(status)) {
        pf->last_error = status;
        return false;
    }

    erase_memory(&buf, sizeof(buf));
    return true;
}

// derive a metadata key from user key (if restore is false, the derived key is randomized)
static bool ipf_generate_secure_blob_from_user_kdk(pf_context_t* pf, bool restore) {
    kdf_input_t buf = {0};
    pf_status_t status;

    DEBUG_PF("pf %p, restore: %d\n", pf, restore);
    buf.index = 1;
    strncpy(buf.label, METADATA_KEY_NAME, strlen(METADATA_KEY_NAME));
    buf.node_number = 0;

    if (!restore) {
        status = cb_random((uint8_t*)&buf.nonce, sizeof(buf.nonce));
        if (PF_FAILURE(status)) {
            pf->last_error = status;
            return false;
        }
    } else {
        COPY_ARRAY(buf.nonce, pf->file_meta_data.plain_part.meta_data_key_id);
    }

    // length of output (128 bits)
    buf.output_len = 0x80;

    status = cb_aes_gcm_encrypt(&pf->user_kdk_key, &g_empty_iv, &buf, sizeof(buf), NULL, 0, NULL,
                                &pf->cur_key);
    if (PF_FAILURE(status)) {
        pf->last_error = status;
        return false;
    }

    if (!restore) {
        COPY_ARRAY(pf->file_meta_data.plain_part.meta_data_key_id, buf.nonce);
    }

    erase_memory(&buf, sizeof(buf));

    return true;
}

// generate a new randomized node master key
static bool ipf_init_session_master_key(pf_context_t* pf) {
    DEBUG_PF("pf %p\n", pf);
    pf_key_t empty_key = {0};

    if (!ipf_generate_secure_blob(pf, &empty_key, MASTER_KEY_NAME, 0, &pf->session_master_key)) {
        return false;
    }

    pf->master_key_count = 0;

    return true;
}

// derive a new randomized node key from master key
static bool ipf_derive_random_node_key(pf_context_t* pf, uint64_t physical_node_number) {
    DEBUG_PF("pf %p, node: %lu\n", pf, physical_node_number);
    if (pf->master_key_count++ >= MAX_MASTER_KEY_USAGES) {
        if (!ipf_init_session_master_key(pf)) {
            return false;
        }
    }

    if (!ipf_generate_secure_blob(pf, &pf->session_master_key, RANDOM_KEY_NAME, physical_node_number,
                                  &pf->cur_key)) {
        return false;
    }

    return true;
}

static bool ipf_generate_random_meta_data_key(pf_context_t* pf) {
    DEBUG_PF("pf %p\n", pf);
    return ipf_generate_secure_blob_from_user_kdk(pf, /*restore=*/false);
}

static bool ipf_restore_current_meta_data_key(pf_context_t* pf) {
    DEBUG_PF("pf %p\n", pf);
    return ipf_generate_secure_blob_from_user_kdk(pf, /*restore=*/true);
}

static bool ipf_init_fields(pf_context_t* pf) {
#ifdef DEBUG
    pf->debug_buffer = malloc(PF_DEBUG_PRINT_SIZE_MAX);
    if (!pf->debug_buffer) {
        pf->last_error = PF_STATUS_NO_MEMORY;
        return false;
    }
#endif
    pf->meta_data_node_number = 0;
    memset(&pf->file_meta_data, 0, sizeof(pf->file_meta_data));
    memset(&pf->encrypted_part_plain, 0, sizeof(pf->encrypted_part_plain));
    memset(&g_empty_iv, 0, sizeof(g_empty_iv));
    memset(&pf->root_mht, 0, sizeof(pf->root_mht));

    pf->root_mht.type = FILE_MHT_NODE_TYPE;
    pf->root_mht.physical_node_number = 1;
    pf->root_mht.node_number = 0;
    pf->root_mht.new_node = true;
    pf->root_mht.need_writing = false;

    pf->offset = 0;
    pf->file = NULL;
    pf->end_of_file = false;
    pf->need_writing = false;
    pf->file_status = PF_STATUS_UNINITIALIZED;
    pf->last_error = PF_STATUS_SUCCESS;
    pf->real_file_size = 0;
    pf->master_key_count = 0;

    pf->recovery_path[0] = '\0';

    pf->cache = lruc_create();
    return true;
}

static pf_context_t* ipf_open(const char* path, pf_file_mode_t mode, bool create,
                              pf_handle_t file, uint64_t real_size, const pf_key_t* kdk_key,
                              bool enable_recovery, pf_status_t* status) {
    *status = PF_STATUS_NO_MEMORY;
    pf_context_t* pf = calloc(1, sizeof(*pf));

    if (!pf)
        goto out;

    if (!ipf_init_fields(pf))
        goto out;

    DEBUG_PF("handle: %d, path: '%s', real size: %lu, mode: 0x%x\n",
             *(int*)file, path, real_size, mode);

    if (kdk_key == NULL) {
        DEBUG_PF("no key specified\n");
        pf->last_error = PF_STATUS_INVALID_PARAMETER;
        goto out;
    }

    if (path && strlen(path) > PATH_MAX_SIZE - 1) {
        pf->last_error = PF_STATUS_PATH_TOO_LONG;
        goto out;
    }

    if (!ipf_init_session_master_key(pf)) {
        // last_error already set
        goto out;
    }

    // for new file, this value will later be saved in the meta data plain part (init_new_file)
    // for existing file, we will later compare this value with the value from the file
    // (init_existing_file)
    COPY_ARRAY(pf->user_kdk_key, *kdk_key);

    // omeg: we require a canonical full path to file, so no stripping path to filename only
    // omeg: Intel's implementation opens the file, we get the fd and size from the Graphene handler

    if (!file) {
        DEBUG_PF("invalid handle\n");
        pf->last_error = PF_STATUS_INVALID_PARAMETER;
        goto out;
    }

    if (real_size % PF_NODE_SIZE != 0) {
        pf->last_error = PF_STATUS_INVALID_HEADER;
        goto out;
    }

    pf->file = file;
    pf->real_file_size = real_size;
    pf->mode = mode;

    if (path && enable_recovery) {
        strncpy(pf->recovery_path, path, sizeof(pf->recovery_path) - 1);
        pf->recovery_path[sizeof(pf->recovery_path) - 1] = '\0'; // just to be safe
        size_t path_len = strnlen(pf->recovery_path, RECOVERY_PATH_MAX_SIZE);
        strncpy(&pf->recovery_path[path_len], "_recovery", 10);
    }

    if (!create) {
        // existing file
        if (!ipf_init_existing_file(pf, path))
            goto out;

    } else {
        // new file
        if (!ipf_init_new_file(pf, path))
            goto out;
    }

    pf->last_error = pf->file_status = PF_STATUS_SUCCESS;
    DEBUG_PF("pf %p, OK (data size %lu)\n", pf, pf->encrypted_part_plain.size);

out:
    if (pf && PF_FAILURE(pf->last_error)) {
        DEBUG_PF("failed: %d\n", pf->last_error);
        free(pf);
        pf = NULL;
    }

    if (pf)
        *status = pf->last_error;

    return pf;
}

static bool ipf_file_recovery(pf_context_t* pf, const char* path) {
    pf_status_t status;
    size_t new_file_size = 0;

    DEBUG_PF("pf %p, path: '%s'\n", pf, path);
    if (!path || strnlen(path, 1) == 0 || !pf->recovery_path
            || strnlen(pf->recovery_path, 1) == 0) {
        pf->last_error = PF_STATUS_RECOVERY_IMPOSSIBLE;
        return false;
    }

    status = cb_close(pf->file);
    if (PF_FAILURE(status)) {
        pf->last_error = status;
        return false;
    }

    pf->file = NULL;

    status = ipf_do_file_recovery(pf, path, PF_NODE_SIZE);
    if (PF_FAILURE(status)) {
        pf->last_error = status;
        return false;
    }

    status = cb_open(path, pf->mode, &pf->file, &new_file_size);
    if (PF_FAILURE(status)) {
        pf->last_error = status;
        return false;
    }

    // recovery only change existing data, it does not shrink or grow the file
    if (new_file_size != pf->real_file_size) {
        pf->last_error = PF_STATUS_UNKNOWN_ERROR;
        return false;
    }

    if (!ipf_read_node(pf, pf->file, /*node_number=*/0, (uint8_t*)&pf->file_meta_data,
                       PF_NODE_SIZE)) {
        return false;
    }

    return true;
}

static bool ipf_read_node(pf_context_t* pf, pf_handle_t handle, uint64_t node_number, void* buffer,
                          uint32_t node_size) {
    uint64_t offset = node_number * node_size;

    DEBUG_PF("pf %p, node %lu, buffer %p, size %u\n", pf, node_number, buffer, node_size);

    pf_status_t status = cb_read(handle, buffer, offset, node_size);
    if (PF_FAILURE(status)) {
        pf->last_error = status;
        return false;
    }

    return true;
}

static bool ipf_write_file(pf_context_t* pf, pf_handle_t handle, uint64_t offset, void* buffer,
                           uint32_t size) {
    DEBUG_PF("pf %p, offset %lu, buffer %p, size %u\n", pf, offset, buffer, size);

    pf_status_t status = cb_write(handle, buffer, offset, size);
    if (PF_FAILURE(status)) {
        pf->last_error = status;
        return false;
    }

    return true;
}

static bool ipf_write_node(pf_context_t* pf, pf_handle_t handle, uint64_t node_number, void* buffer,
                           uint32_t node_size) {
    DEBUG_PF("pf %p, node %lu, buf %p, size %u\n", pf, node_number, buffer, node_size);
    return ipf_write_file(pf, handle, node_number * node_size, buffer, node_size);
}

static bool ipf_init_existing_file(pf_context_t* pf, const char* path) {
    pf_status_t status;

    DEBUG_PF("pf %p, path '%s'\n", pf, path);
    // read meta-data node
    if (!ipf_read_node(pf, pf->file, /*node_number=*/0, (uint8_t*)&pf->file_meta_data,
                       PF_NODE_SIZE)) {
        return false;
    }

    if (pf->file_meta_data.plain_part.file_id != PF_FILE_ID) {
        // such a file exists, but it is not a protected file
        pf->last_error = PF_STATUS_INVALID_HEADER;
        return false;
    }

    if (pf->file_meta_data.plain_part.major_version != PF_MAJOR_VERSION) {
        pf->last_error = PF_STATUS_INVALID_VERSION;
        return false;
    }

    if (pf->file_meta_data.plain_part.update_flag == 1) {
        // file was in the middle of an update, must do a recovery
        if (!ipf_file_recovery(pf, path)) {
            if (pf->last_error == PF_STATUS_RECOVERY_IMPOSSIBLE) {
                DEBUG_PF("file recovery impossible\n");
                return false;
            }
            // override internal error
            pf->last_error = PF_STATUS_RECOVERY_NEEDED;
            return false;
        }

        if (pf->file_meta_data.plain_part.update_flag == 1) {
            // recovery failed, flag is still set!
            pf->last_error = PF_STATUS_RECOVERY_NEEDED;
            return false;
        }

        // re-check after recovery
        if (pf->file_meta_data.plain_part.major_version != PF_MAJOR_VERSION) {
            pf->last_error = PF_STATUS_INVALID_VERSION;
            return false;
        }
    }

    if (!ipf_restore_current_meta_data_key(pf))
        return false;

    // decrypt the encrypted part of the meta-data
    status = cb_aes_gcm_decrypt(&pf->cur_key, &g_empty_iv, NULL, 0,
                                &pf->file_meta_data.encrypted_part,
                                sizeof(pf->file_meta_data.encrypted_part),
                                &pf->encrypted_part_plain,
                                &pf->file_meta_data.plain_part.meta_data_gmac);
    if (PF_FAILURE(status)) {
        pf->last_error = status;
        DEBUG_PF("failed to decrypt metadata: %d\n", status);
        return false;
    }

    DEBUG_PF("data size %lu\n", pf->encrypted_part_plain.size);

    if (path) {
        size_t name_len = strlen(pf->encrypted_part_plain.clean_filename);
        if (name_len != strlen(path)
                || memcmp(path, pf->encrypted_part_plain.clean_filename, name_len) != 0) {
            pf->last_error = PF_STATUS_INVALID_PATH;
            return false;
        }
    }

    if (pf->encrypted_part_plain.size > MD_USER_DATA_SIZE) {
        // read the root node of the mht
        if (!ipf_read_node(pf, pf->file, /*node_number=*/1, &pf->root_mht.encrypted.cipher,
                           PF_NODE_SIZE))
            return false;

        // this also verifies the root mht gmac against the gmac in the meta-data encrypted part
        status = cb_aes_gcm_decrypt(&pf->encrypted_part_plain.mht_key, &g_empty_iv,
                                    NULL, 0, // aad
                                    &pf->root_mht.encrypted.cipher, PF_NODE_SIZE,
                                    &pf->root_mht.decrypted.mht,
                                    &pf->encrypted_part_plain.mht_gmac);
        if (PF_FAILURE(status)) {
            pf->last_error = status;
            return false;
        }

        pf->root_mht.new_node = false;
    }

    return true;
}

static bool ipf_init_new_file(pf_context_t* pf, const char* path) {
    DEBUG_PF("pf %p, filename '%s'\n", pf, path);
    pf->file_meta_data.plain_part.file_id = PF_FILE_ID;
    pf->file_meta_data.plain_part.major_version = PF_MAJOR_VERSION;
    pf->file_meta_data.plain_part.minor_version = PF_MINOR_VERSION;

    strncpy(pf->encrypted_part_plain.clean_filename, path, PATH_MAX_SIZE);

    pf->need_writing = true;

    return true;
}

static bool ipf_close(pf_context_t* pf) {
    void* data;
    bool retval = true;

    DEBUG_PF("pf %p\n", pf);

    if (pf->file_status != PF_STATUS_SUCCESS) {
        ipf_try_clear_error(pf); // last attempt to fix it
        retval = false;
    } else {
        if (!ipf_internal_flush(pf)) {
            DEBUG_PF("internal flush failed\n");
            retval = false;
        }
    }

    if (pf->file_status == PF_STATUS_SUCCESS && pf->last_error == PF_STATUS_SUCCESS)
        ipf_erase_recovery_file(pf);

    // omeg: fs close is done by Graphene handler
    pf->file_status = PF_STATUS_UNINITIALIZED;

    while ((data = lruc_get_last(pf->cache)) != NULL) {
        file_node_t* file_node = (file_node_t*)data;
        erase_memory(&file_node->decrypted, sizeof(file_node->decrypted));
        free(file_node);
        lruc_remove_last(pf->cache);
    }

    // scrub the last encryption key and the session key
    erase_memory(&pf->cur_key, sizeof(pf->cur_key));
    erase_memory(&pf->session_master_key, sizeof(pf->session_master_key));

    // scrub first 3KB of user data and the gmac_key
    erase_memory(&pf->encrypted_part_plain, sizeof(pf->encrypted_part_plain));

    lruc_destroy(pf->cache);

#ifdef DEBUG
    free(pf->debug_buffer);
#endif
    erase_memory(pf, sizeof(struct pf_context));
    free(pf);

    return retval;
}

static bool ipf_internal_flush(pf_context_t* pf) {
    DEBUG_PF("pf %p\n", pf);
    if (!pf->need_writing) {
        // no changes at all
        DEBUG_PF("no need to write\n");
        return true;
    }

    if (pf->encrypted_part_plain.size > MD_USER_DATA_SIZE && pf->root_mht.need_writing) {
        // otherwise it's just one write - the meta-data node
        if (pf->recovery_path && pf->recovery_path[0]) {
            if (!ipf_write_recovery_file(pf)) {
                pf->file_status = PF_STATUS_FLUSH_ERROR;
                DEBUG_PF("failed to write recovery file\n");
                return false;
            }
        }

        if (!ipf_set_update_flag(pf)) {
            pf->file_status = PF_STATUS_FLUSH_ERROR;
            DEBUG_PF("failed to set update flag\n");
            return false;
        }

        if (!ipf_update_all_data_and_mht_nodes(pf)) {
            ipf_clear_update_flag(pf);
            // this is something that shouldn't happen, can't fix this...
            pf->file_status = PF_STATUS_CRYPTO_ERROR;
            DEBUG_PF("failed to update data nodes\n");
            return false;
        }
    }

    if (!ipf_update_meta_data_node(pf)) {
        ipf_clear_update_flag(pf);
        // this is something that shouldn't happen, can't fix this...
        pf->file_status = PF_STATUS_CRYPTO_ERROR;
        DEBUG_PF("failed to update metadata nodes\n");
        return false;
    }

    if (!ipf_write_all_changes_to_disk(pf)) {
        pf->file_status = PF_STATUS_WRITE_TO_DISK_FAILED;

        DEBUG_PF("failed to write changes to disk\n");
        return false;
    }

    pf->need_writing = false;

    return true;
}

static bool ipf_write_recovery_file(pf_context_t* pf) {
    pf_handle_t recovery_file = NULL;

    DEBUG_PF("pf %p\n", pf);
    if (!pf->recovery_path || !pf->recovery_path[0]) {
        pf->last_error = PF_STATUS_RECOVERY_IMPOSSIBLE;
        return false;
    }

    pf_status_t status = cb_open(pf->recovery_path, PF_FILE_MODE_WRITE, &recovery_file, NULL);
    if (PF_FAILURE(status)) {
        pf->last_error = status;
        return false;
    }

    void* data = NULL;
    recovery_node_t* recovery_node = NULL;

    uint64_t node_number = 0;
    for (data = lruc_get_first(pf->cache); data != NULL; data = lruc_get_next(pf->cache)) {
        file_node_t* file_node = (file_node_t*)data;
        if (!file_node->need_writing || file_node->new_node)
            continue;

        recovery_node = &file_node->recovery_node;

        if (!ipf_write_node(pf, recovery_file, node_number, recovery_node, sizeof(*recovery_node))) {
            cb_close(recovery_file);
            cb_delete(pf->recovery_path);
            return false;
        }

        node_number++;
    }

    /* Intel's implementation writes recovery nodes sequentially, do the same */
    uint64_t offset = node_number * sizeof(*recovery_node);
    if (pf->root_mht.need_writing && !pf->root_mht.new_node) {
        if (!ipf_write_file(pf, recovery_file, offset, &pf->root_mht.recovery_node,
                            sizeof(pf->root_mht.recovery_node))) {
            cb_close(recovery_file);
            cb_delete(pf->recovery_path);
            return false;
        }

        offset += sizeof(pf->root_mht.recovery_node);
    }

    if (!ipf_write_file(pf, recovery_file, offset, &pf->meta_data_recovery_node,
                        sizeof(pf->meta_data_recovery_node))) {
        cb_close(recovery_file);
        cb_delete(pf->recovery_path);
        return false;
    }

    status = cb_close(recovery_file);
    if (PF_FAILURE(status)) {
        pf->last_error = status;
        return false;
    }

    return true;
}

static bool ipf_set_update_flag(pf_context_t* pf) {
    DEBUG_PF("pf %p\n", pf);
    pf->file_meta_data.plain_part.update_flag = 1;
    if (!ipf_write_node(pf, pf->file, /*node_number=*/0, &pf->file_meta_data, PF_NODE_SIZE)) {
        return false;
    }
    // turn it off in memory. at the end of the flush, when we'll write the meta-data to disk,
    // this flag will also be cleared there.
    pf->file_meta_data.plain_part.update_flag = 0;

    return true;
}

// this function is called if we had an error after we updated the update flag
// in normal flow, the flag is cleared when the meta-data is written to disk
static void ipf_clear_update_flag(pf_context_t* pf) {
    DEBUG_PF("pf %p\n", pf);
    assert(pf->file_meta_data.plain_part.update_flag == 0);
    ipf_write_node(pf, pf->file, 0, &pf->file_meta_data, PF_NODE_SIZE);
}

// sort function, we need the mht nodes sorted before we start to update their gmac's
static bool mht_sort(const void* first, const void* second) {
    // higher (lower tree level) node number first
    if (!first || !second)
        return false;

    return (((file_node_t*)first)->node_number >
            ((file_node_t*)second)->node_number);
}

// TODO: faster sort
#define LISTP_SORT(LISTP, STRUCT_NAME, SORT_FUNC)                                       \
    do {                                                                                \
        struct STRUCT_NAME* head  = NULL;                                               \
        struct STRUCT_NAME* outer = NULL;                                               \
        struct STRUCT_NAME* inner = NULL;                                               \
        head                = LISTP_FIRST_ENTRY(LISTP, STRUCT_NAME, list);              \
        outer               = head;                                                     \
        bool adjacent_nodes = false;                                                    \
        while (outer != NULL) {                                                         \
            inner = LISTP_NEXT_ENTRY(outer, LISTP, list);                               \
            while (inner != NULL) {                                                     \
                if (!SORT_FUNC(outer, inner)) {                                         \
                    struct STRUCT_NAME* for_swap = NULL;                                \
                    adjacent_nodes =                                                    \
                        (LISTP_NEXT_ENTRY(outer, LISTP, list) == inner) ? true : false; \
                    if (!adjacent_nodes) {                                              \
                        struct STRUCT_NAME* prev_of_inner =                             \
                            LISTP_PREV_ENTRY(inner, LISTP, list);                       \
                        struct STRUCT_NAME* prev_of_outer =                             \
                            LISTP_PREV_ENTRY(outer, LISTP, list);                       \
                        LISTP_DEL(inner, LISTP, list);                                  \
                        LISTP_DEL(outer, LISTP, list);                                  \
                        LISTP_ADD_AFTER(inner, prev_of_outer, LISTP, list);             \
                        LISTP_ADD_AFTER(outer, prev_of_inner, LISTP, list);             \
                    } else {                                                            \
                        LISTP_DEL(outer, LISTP, list);                                  \
                        LISTP_ADD_AFTER(outer, inner, LISTP, list);                     \
                    }                                                                   \
                    for_swap = inner;                                                   \
                    inner    = outer;                                                   \
                    outer    = for_swap;                                                \
                }                                                                       \
                inner = LISTP_NEXT_ENTRY(inner, LISTP, list);                           \
            }                                                                           \
            outer = LISTP_NEXT_ENTRY(outer, LISTP, list);                               \
        }                                                                               \
    } while (0)

static bool ipf_update_all_data_and_mht_nodes(pf_context_t* pf) {
    DEBUG_PF("pf %p\n", pf);
    LISTP_TYPE(_file_node) mht_list = LISTP_INIT;
    file_node_t* file_mht_node;
    pf_status_t status;
    void* data = lruc_get_first(pf->cache);

    // 1. encrypt the changed data
    // 2. set the IV+GMAC in the parent MHT
    // [3. set the need_writing flag for all the parents]
    while (data != NULL) {
        if (((file_node_t*)data)->type == FILE_DATA_NODE_TYPE) {
            file_node_t* data_node = (file_node_t*)data;

            if (data_node->need_writing) {
                if (!ipf_derive_random_node_key(pf, data_node->physical_node_number))
                    return false;

                gcm_crypto_data_t* gcm_crypto_data = &data_node->parent->decrypted.mht
                    .data_nodes_crypto[data_node->node_number % ATTACHED_DATA_NODES_COUNT];

                // encrypt the data, this also saves the gmac of the operation in the mht crypto node
                status = cb_aes_gcm_encrypt(&pf->cur_key, &g_empty_iv,
                                            NULL, 0, // aad
                                            data_node->decrypted.data.data, PF_NODE_SIZE,
                                            data_node->encrypted.cipher,
                                            &gcm_crypto_data->gmac);
                if (PF_FAILURE(status)) {
                    pf->last_error = status;
                    return false;
                }

                // save the key used for this encryption
                COPY_ARRAY(gcm_crypto_data->key, pf->cur_key);

                file_mht_node = data_node->parent;
#ifdef DEBUG
                // this loop should do nothing, add it here just to be safe
                while (file_mht_node->node_number != 0) {
                    assert(file_mht_node->need_writing == true);
                    file_mht_node = file_mht_node->parent;
                }
#endif
            }
        }
        data = lruc_get_next(pf->cache);
    }

    // add all the mht nodes that needs writing to a list
    data = lruc_get_first(pf->cache);
    while (data != NULL) {
        if (((file_node_t*)data)->type == FILE_MHT_NODE_TYPE) {
            // type is in the same offset in both node types
            file_mht_node = (file_node_t*)data;

            if (file_mht_node->need_writing)
                LISTP_ADD(file_mht_node, &mht_list, list);
        }

        data = lruc_get_next(pf->cache);
    }

    // sort the list from the last node to the first (bottom layers first)
    LISTP_SORT(&mht_list, _file_node, mht_sort);

    // update the gmacs in the parents
    struct _file_node* node;
    struct _file_node* tmp;

    LISTP_FOR_EACH_ENTRY_SAFE(node, tmp, &mht_list, list) {
        file_mht_node = node;

        gcm_crypto_data_t* gcm_crypto_data = &file_mht_node->parent->decrypted.mht
            .mht_nodes_crypto[(file_mht_node->node_number - 1) % CHILD_MHT_NODES_COUNT];

        if (!ipf_derive_random_node_key(pf, file_mht_node->physical_node_number)) {
            return false;
        }

        status = cb_aes_gcm_encrypt(&pf->cur_key, &g_empty_iv,
                                    NULL, 0,
                                    &file_mht_node->decrypted.mht, PF_NODE_SIZE,
                                    &file_mht_node->encrypted.cipher,
                                    &gcm_crypto_data->gmac);
        if (PF_FAILURE(status)) {
            pf->last_error = status;
            return false;
        }

        // save the key used for this gmac
        COPY_ARRAY(gcm_crypto_data->key, pf->cur_key);

        LISTP_DEL(node, &mht_list, list);
    }

    // update mht root gmac in the meta data node
    if (!ipf_derive_random_node_key(pf, pf->root_mht.physical_node_number))
        return false;

    status = cb_aes_gcm_encrypt(&pf->cur_key, &g_empty_iv,
                                NULL, 0,
                                &pf->root_mht.decrypted.mht, PF_NODE_SIZE,
                                &pf->root_mht.encrypted.cipher,
                                &pf->encrypted_part_plain.mht_gmac);
    if (PF_FAILURE(status)) {
        pf->last_error = status;
        return false;
    }

    // save the key used for this gmac
    COPY_ARRAY(pf->encrypted_part_plain.mht_key, pf->cur_key);

    return true;
}

static bool ipf_update_meta_data_node(pf_context_t* pf) {
    pf_status_t status;

    DEBUG_PF("pf %p\n", pf);
    // randomize a new key, saves the key _id_ in the meta data plain part
    if (!ipf_generate_random_meta_data_key(pf)) {
        // last error already set
        return false;
    }

    // encrypt meta data encrypted part, also updates the gmac in the meta data plain part
    status = cb_aes_gcm_encrypt(&pf->cur_key, &g_empty_iv,
                                NULL, 0,
                                &pf->encrypted_part_plain, sizeof(meta_data_encrypted_t),
                                &pf->file_meta_data.encrypted_part,
                                &pf->file_meta_data.plain_part.meta_data_gmac);
    if (PF_FAILURE(status)) {
        pf->last_error = status;
        return false;
    }

    return true;
}

static bool ipf_write_all_changes_to_disk(pf_context_t* pf) {
    DEBUG_PF("pf %p\n", pf);
    if (pf->encrypted_part_plain.size > MD_USER_DATA_SIZE && pf->root_mht.need_writing) {
        void* data = NULL;
        uint8_t* data_to_write;
        uint64_t node_number;
        file_node_t* file_node;

        for (data = lruc_get_first(pf->cache); data != NULL; data = lruc_get_next(pf->cache)) {
            file_node = (file_node_t*)data;
            if (!file_node->need_writing)
                continue;

            data_to_write = (uint8_t*)&file_node->encrypted;
            node_number = file_node->physical_node_number;
            DEBUG_PF("node %lu, type %d, parent %p\n",
                     file_node->node_number, file_node->type, file_node->parent);

            if (!ipf_write_node(pf, pf->file, node_number, data_to_write, PF_NODE_SIZE)) {
                return false;
            }

            // data written - clear the need_writing and the new_node flags
            // (for future transactions, this node it no longer 'new' and should be written
            // to recovery file)
            file_node->need_writing = false;
            file_node->new_node = false;
        }

        if (!ipf_write_node(pf, pf->file, /*node_number=*/1, &pf->root_mht.encrypted,
                            PF_NODE_SIZE)) {
            return false;
        }

        pf->root_mht.need_writing = false;
        pf->root_mht.new_node = false;
    }

    if (!ipf_write_node(pf, pf->file, /*node_number=*/0, &pf->file_meta_data, PF_NODE_SIZE)) {
        return false;
    }

    return true;
}

static bool ipf_erase_recovery_file(pf_context_t* pf) {
    pf_status_t status;

    DEBUG_PF("pf %p\n", pf);
    if (!pf->recovery_path) {
        pf->last_error = PF_STATUS_RECOVERY_IMPOSSIBLE;
        return false;
    }

    if (pf->recovery_path[0] == '\0') // not initialized yet
        return true;

    status = cb_delete(pf->recovery_path);
    if (PF_FAILURE(status)) {
        pf->last_error = status;
        return false;
    }

    return true;
}

// seek to a specified file offset from the beginning
// seek beyond the current size is supported if the file is writable,
// the file is then extended with zeros (Intel SGX SDK implementation didn't support extending)
static bool ipf_seek(pf_context_t* pf, uint64_t new_offset) {
    DEBUG_PF("pf %p, size %lu, offset %ld\n", pf, pf->encrypted_part_plain.size, new_offset);
    if (PF_FAILURE(pf->file_status)) {
        pf->last_error = pf->file_status;
        return false;
    }

    bool result = false;

    if (new_offset <= pf->encrypted_part_plain.size) {
        pf->offset = new_offset;
        result = true;
    } else if (pf->mode & PF_FILE_MODE_WRITE) {
        // need to extend the file
        result = PF_SUCCESS(pf_set_size(pf, new_offset));
    }

    if (result)
        pf->end_of_file = false;
    else
        pf->last_error = PF_STATUS_INVALID_PARAMETER;

    return result;
}

static void ipf_try_clear_error(pf_context_t* pf) {
    DEBUG_PF("pf %p\n", pf);
    if (pf->file_status == PF_STATUS_UNINITIALIZED ||
        pf->file_status == PF_STATUS_CRYPTO_ERROR ||
        pf->file_status == PF_STATUS_CORRUPTED) {
        // can't fix these...
        DEBUG_PF("Unrecoverable file status: %d\n", pf->file_status);
        return;
    }

    if (pf->file_status == PF_STATUS_FLUSH_ERROR) {
        if (ipf_internal_flush(pf))
            pf->file_status = PF_STATUS_SUCCESS;
    }

    if (pf->file_status == PF_STATUS_WRITE_TO_DISK_FAILED) {
        if (ipf_write_all_changes_to_disk(pf)) {
            pf->need_writing = false;
            pf->file_status = PF_STATUS_SUCCESS;
        }
    }

    if (pf->file_status == PF_STATUS_SUCCESS) {
        pf->last_error = PF_STATUS_SUCCESS;
        pf->end_of_file = false;
    }
}

// memcpy src->dest if src is not NULL, zero dest otherwise
static void memcpy_or_zero_initialize(void* dest, const void* src, size_t size) {
    if (src)
        memcpy(dest, src, size);
    else
        memset(dest, 0, size);
}

// write zeros if `ptr` is NULL
static size_t ipf_write(pf_context_t* pf, const void* ptr, size_t size) {
    if (size == 0) {
        pf->last_error = PF_STATUS_INVALID_PARAMETER;
        return 0;
    }

    size_t data_left_to_write = size;
    DEBUG_PF("pf %p, buf %p, size %lu\n", pf, ptr, size);

    if (PF_FAILURE(pf->file_status)) {
        pf->last_error = pf->file_status;
        DEBUG_PF("bad file status %d\n", pf->last_error);
        return 0;
    }

    if (!(pf->mode & PF_FILE_MODE_WRITE)) {
        pf->last_error = PF_STATUS_INVALID_MODE;
        DEBUG_PF("File is read-only\n");
        return 0;
    }

    const unsigned char* data_to_write = (const unsigned char*)ptr;

    // the first block of user data is written in the meta-data encrypted part
    if (pf->offset < MD_USER_DATA_SIZE) {
        // offset is smaller than MD_USER_DATA_SIZE
        size_t empty_place_left_in_md = MD_USER_DATA_SIZE - (size_t)pf->offset;
        size_t size_to_write = MIN(data_left_to_write, empty_place_left_in_md);

        memcpy_or_zero_initialize(&pf->encrypted_part_plain.data[pf->offset], data_to_write,
                                  size_to_write);
        pf->offset += size_to_write;
        if (data_to_write)
            data_to_write += size_to_write;
        data_left_to_write -= size_to_write;

        if (pf->offset > pf->encrypted_part_plain.size)
            pf->encrypted_part_plain.size = pf->offset; // file grew, update the new file size

        pf->need_writing = true;
    }

    while (data_left_to_write > 0) {
        file_node_t* file_data_node = NULL;
        // return the data node of the current offset, will read it from disk or create new one
        // if needed (and also the mht node if needed)
        file_data_node = ipf_get_data_node(pf);
        if (file_data_node == NULL) {
            DEBUG_PF("failed to get data node\n");
            break;
        }

        size_t offset_in_node = (size_t)((pf->offset - MD_USER_DATA_SIZE) % PF_NODE_SIZE);
        size_t empty_place_left_in_node = PF_NODE_SIZE - offset_in_node;
        size_t size_to_write = MIN(data_left_to_write, empty_place_left_in_node);

        memcpy_or_zero_initialize(&file_data_node->decrypted.data.data[offset_in_node],
                                  data_to_write, size_to_write);
        pf->offset += size_to_write;
        if (data_to_write)
            data_to_write += size_to_write;
        data_left_to_write -= size_to_write;

        if (pf->offset > pf->encrypted_part_plain.size) {
            pf->encrypted_part_plain.size = pf->offset; // file grew, update the new file size
        }

        if (!file_data_node->need_writing) {
            file_data_node->need_writing = true;
            file_node_t* file_mht_node = file_data_node->parent;
            while (file_mht_node->node_number != 0) {
                // set all the mht parent nodes as 'need writing'
                file_mht_node->need_writing = true;
                file_mht_node = file_mht_node->parent;
            }
            pf->root_mht.need_writing = true;
            pf->need_writing = true;
        }
    }

    size_t written = size - data_left_to_write;
    DEBUG_PF("returning %lu\n", written);
    return written;
}

static size_t ipf_read(pf_context_t* pf, void* ptr, size_t size) {
    if (ptr == NULL || size == 0)
        return 0;

    size_t data_left_to_read = size;
    DEBUG_PF("pf %p, buf %p, size %lu\n", pf, ptr, size);

    if (PF_FAILURE(pf->file_status)) {
        pf->last_error = pf->file_status;
        return 0;
    }

    if (!(pf->mode & PF_FILE_MODE_READ)) {
        pf->last_error = PF_STATUS_INVALID_MODE;
        return 0;
    }

    if (pf->end_of_file) {
        // not an error
        return 0;
    }

    // this check is not really needed, can go on with the code and it will do nothing until the end,
    // but it's more 'right' to check it here
    if (pf->offset == pf->encrypted_part_plain.size) {
        pf->end_of_file = true;
        return 0;
    }

    if (((uint64_t)data_left_to_read) > (uint64_t)(pf->encrypted_part_plain.size - pf->offset)) {
        // the request is bigger than what's left in the file
        data_left_to_read = (size_t)(pf->encrypted_part_plain.size - pf->offset);
    }

    // used at the end to return how much we actually read
    size_t data_attempted_to_read = data_left_to_read;

    unsigned char* out_buffer = (unsigned char*)ptr;

    // the first block of user data is read from the meta-data encrypted part
    if (pf->offset < MD_USER_DATA_SIZE) {
        // offset is smaller than MD_USER_DATA_SIZE
        size_t data_left_in_md = MD_USER_DATA_SIZE - (size_t)pf->offset;
        size_t size_to_read = MIN(data_left_to_read, data_left_in_md);

        memcpy(out_buffer, &pf->encrypted_part_plain.data[pf->offset], size_to_read);
        pf->offset += size_to_read;
        out_buffer += size_to_read;
        data_left_to_read -= size_to_read;
    }

    while (data_left_to_read > 0) {
        file_node_t* file_data_node = NULL;
        // return the data node of the current offset, will read it from disk if needed
        // (and also the mht node if needed)
        file_data_node = ipf_get_data_node(pf);
        if (file_data_node == NULL)
            break;

        size_t offset_in_node = (pf->offset - MD_USER_DATA_SIZE) % PF_NODE_SIZE;
        size_t data_left_in_node = PF_NODE_SIZE - offset_in_node;
        size_t size_to_read = MIN(data_left_to_read, data_left_in_node);

        memcpy(out_buffer, &file_data_node->decrypted.data.data[offset_in_node],
               size_to_read);
        pf->offset += size_to_read;
        out_buffer += size_to_read;
        data_left_to_read -= size_to_read;
    }

    if (data_left_to_read == 0 && data_attempted_to_read != size) {
        // user wanted to read more and we had to shrink the request
        assert(pf->offset == pf->encrypted_part_plain.size);
        pf->end_of_file = true;
    }

    return data_attempted_to_read - data_left_to_read;
}

// this is a very 'specific' function, tied to the architecture of the file layout,
// returning the node numbers according to the data offset in the file
static void get_node_numbers(uint64_t offset, uint64_t* mht_node_number, uint64_t* data_node_number,
                             uint64_t* physical_mht_node_number,
                             uint64_t* physical_data_node_number) {
    // physical nodes (file layout):
    // node 0 - meta data node
    // node 1 - mht
    // nodes 2-97 - data (ATTACHED_DATA_NODES_COUNT == 96)
    // node 98 - mht
    // node 99-195 - data
    // etc.
    uint64_t _physical_mht_node_number;
    uint64_t _physical_data_node_number;

    // "logical" nodes: sequential index of the corresponding mht/data node in all mht/data nodes
    uint64_t _mht_node_number;
    uint64_t _data_node_number;

    assert(offset >= MD_USER_DATA_SIZE);

    _data_node_number = (offset - MD_USER_DATA_SIZE) / PF_NODE_SIZE;
    _mht_node_number = _data_node_number / ATTACHED_DATA_NODES_COUNT;
    _physical_data_node_number = _data_node_number
                                 + 1 // meta data node
                                 + 1 // mht root
                                 + _mht_node_number; // number of mht nodes in the middle
                                 // (the root mht mht_node_number is 0)
    _physical_mht_node_number = _physical_data_node_number
                                - _data_node_number % ATTACHED_DATA_NODES_COUNT // now we are at
                                // the first data node attached to this mht node
                                - 1; // and now at the mht node itself!

    if (mht_node_number != NULL)
        *mht_node_number = _mht_node_number;
    if (data_node_number != NULL)
        *data_node_number = _data_node_number;
    if (physical_mht_node_number != NULL)
        *physical_mht_node_number = _physical_mht_node_number;
    if (physical_data_node_number != NULL)
        *physical_data_node_number = _physical_data_node_number;
}

static file_node_t* ipf_get_data_node(pf_context_t* pf) {
    file_node_t* file_data_node = NULL;

    DEBUG_PF("pf %p\n", pf);
    if (pf->offset < MD_USER_DATA_SIZE) {
        pf->last_error = PF_STATUS_UNKNOWN_ERROR;
        return NULL;
    }

    if ((pf->offset - MD_USER_DATA_SIZE) % PF_NODE_SIZE == 0
        && pf->offset == pf->encrypted_part_plain.size) {
        // new node
        file_data_node = ipf_append_data_node(pf);
    } else {
        // existing node
        file_data_node = ipf_read_data_node(pf);
    }

    // bump all the parents mht to reside before the data node in the cache
    if (file_data_node != NULL) {
        file_node_t* file_mht_node = file_data_node->parent;
        while (file_mht_node->node_number != 0) {
            // bump the mht node to the head of the lru
            lruc_get(pf->cache, file_mht_node->physical_node_number);
            file_mht_node = file_mht_node->parent;
        }
    }

    // even if we didn't get the required data_node, we might have read other nodes in the process
    while (lruc_size(pf->cache) > MAX_PAGES_IN_CACHE) {
        void* data = lruc_get_last(pf->cache);
        assert(data != NULL);
        // for production -
        if (data == NULL) {
            pf->last_error = PF_STATUS_UNKNOWN_ERROR;
            return NULL;
        }

        if (!((file_node_t*)data)->need_writing) {
            lruc_remove_last(pf->cache);

            // before deleting the memory, need to scrub the plain secrets
            file_node_t* file_node = (file_node_t*)data;
            erase_memory(&file_node->decrypted, sizeof(file_node->decrypted));
            free(file_node);
        } else {
            if (!ipf_internal_flush(pf)) {
                // error, can't flush cache, file status changed to error
                assert(pf->file_status != PF_STATUS_SUCCESS);
                if (pf->file_status == PF_STATUS_SUCCESS)
                    pf->file_status = PF_STATUS_FLUSH_ERROR; // for release set this anyway
                return NULL; // even if we got the data_node!
            }
        }
    }

    return file_data_node;
}

static file_node_t* ipf_append_data_node(pf_context_t* pf) {
    DEBUG_PF("pf %p\n", pf);

    file_node_t* file_mht_node = ipf_get_mht_node(pf);
    if (file_mht_node == NULL) // some error happened
        return NULL;

    file_node_t* new_file_data_node = NULL;

    new_file_data_node = calloc(1, sizeof(*new_file_data_node));
    if (!new_file_data_node) {
        pf->last_error = PF_STATUS_NO_MEMORY;
        return NULL;
    }

    new_file_data_node->type = FILE_DATA_NODE_TYPE;
    new_file_data_node->new_node = true;
    new_file_data_node->parent = file_mht_node;
    get_node_numbers(pf->offset, NULL, &new_file_data_node->node_number, NULL,
                     &new_file_data_node->physical_node_number);

    if (!lruc_add(pf->cache, new_file_data_node->physical_node_number, new_file_data_node)) {
        free(new_file_data_node);
        pf->last_error = PF_STATUS_NO_MEMORY;
        return NULL;
    }

    return new_file_data_node;
}

static file_node_t* ipf_read_data_node(pf_context_t* pf) {
    DEBUG_PF("pf %p\n", pf);

    uint64_t data_node_number;
    uint64_t physical_node_number;
    file_node_t* file_mht_node;
    pf_status_t status;

    get_node_numbers(pf->offset, NULL, &data_node_number, NULL, &physical_node_number);

    file_node_t* file_data_node = (file_node_t*)lruc_get(pf->cache, physical_node_number);
    if (file_data_node != NULL)
        return file_data_node;

    // need to read the data node from the disk

    file_mht_node = ipf_get_mht_node(pf);
    if (file_mht_node == NULL) // some error happened
        return NULL;

    file_data_node = calloc(1, sizeof(*file_data_node));
    if (!file_data_node) {
        pf->last_error = PF_STATUS_NO_MEMORY;
        return NULL;
    }

    file_data_node->type = FILE_DATA_NODE_TYPE;
    file_data_node->node_number = data_node_number;
    file_data_node->physical_node_number = physical_node_number;
    file_data_node->parent = file_mht_node;

    if (!ipf_read_node(pf, pf->file, file_data_node->physical_node_number,
                       file_data_node->encrypted.cipher, PF_NODE_SIZE)) {
        free(file_data_node);
        return NULL;
    }

    gcm_crypto_data_t* gcm_crypto_data = &file_data_node->parent->decrypted.mht
        .data_nodes_crypto[file_data_node->node_number % ATTACHED_DATA_NODES_COUNT];

    // this function decrypt the data _and_ checks the integrity of the data against the gmac
    status = cb_aes_gcm_decrypt(&gcm_crypto_data->key, &g_empty_iv,
                                NULL, 0,
                                file_data_node->encrypted.cipher, PF_NODE_SIZE,
                                file_data_node->decrypted.data.data,
                                &gcm_crypto_data->gmac);

    if (PF_FAILURE(status)) {
        free(file_data_node);
        pf->last_error = status;
        if (status == PF_STATUS_MAC_MISMATCH)
            pf->file_status = PF_STATUS_CORRUPTED;
        return NULL;
    }

    if (!lruc_add(pf->cache, file_data_node->physical_node_number, file_data_node)) {
        // scrub the plaintext data
        erase_memory(&file_data_node->decrypted, sizeof(file_data_node->decrypted));
        free(file_data_node);
        pf->last_error = PF_STATUS_NO_MEMORY;
        return NULL;
    }

    return file_data_node;
}

static file_node_t* ipf_get_mht_node(pf_context_t* pf) {
    DEBUG_PF("pf %p\n", pf);

    file_node_t* file_mht_node;
    uint64_t mht_node_number;
    uint64_t physical_mht_node_number;

    if (pf->offset < MD_USER_DATA_SIZE) {
        pf->last_error = PF_STATUS_UNKNOWN_ERROR;
        return NULL;
    }

    get_node_numbers(pf->offset, &mht_node_number, NULL, &physical_mht_node_number, NULL);

    if (mht_node_number == 0)
        return &pf->root_mht;

    // file is constructed from (ATTACHED_DATA_NODES_COUNT + CHILD_MHT_NODES_COUNT) * PF_NODE_SIZE
    // bytes per MHT node
    if ((pf->offset - MD_USER_DATA_SIZE) % (ATTACHED_DATA_NODES_COUNT * PF_NODE_SIZE) == 0 &&
         pf->offset == pf->encrypted_part_plain.size) {
        file_mht_node = ipf_append_mht_node(pf, mht_node_number);
    } else {
        file_mht_node = ipf_read_mht_node(pf, mht_node_number);
    }

    return file_mht_node;
}

static file_node_t* ipf_append_mht_node(pf_context_t* pf, uint64_t mht_node_number) {
    DEBUG_PF("pf %p, node %lu\n", pf, mht_node_number);

    assert(mht_node_number > 0);
    file_node_t* parent_file_mht_node =
        ipf_read_mht_node(pf, (mht_node_number - 1) / CHILD_MHT_NODES_COUNT);

    if (parent_file_mht_node == NULL) // some error happened
        return NULL;

    uint64_t physical_node_number = 1 + // meta data node
                                    // the '1' is for the mht node preceding every 96 data nodes
                                    mht_node_number * (1 + ATTACHED_DATA_NODES_COUNT);

    file_node_t* new_file_mht_node = NULL;
    new_file_mht_node = calloc(1, sizeof(*new_file_mht_node));
    if (!new_file_mht_node) {
        pf->last_error = PF_STATUS_NO_MEMORY;
        return NULL;
    }

    new_file_mht_node->type = FILE_MHT_NODE_TYPE;
    new_file_mht_node->new_node = true;
    new_file_mht_node->parent = parent_file_mht_node;
    new_file_mht_node->node_number = mht_node_number;
    new_file_mht_node->physical_node_number = physical_node_number;

    if (!lruc_add(pf->cache, new_file_mht_node->physical_node_number, new_file_mht_node)) {
        free(new_file_mht_node);
        pf->last_error = PF_STATUS_NO_MEMORY;
        return NULL;
    }

    return new_file_mht_node;
}

static file_node_t* ipf_read_mht_node(pf_context_t* pf, uint64_t mht_node_number) {
    pf_status_t status;

    DEBUG_PF("pf %p, node %lu\n", pf, mht_node_number);
    if (mht_node_number == 0)
        return &pf->root_mht;

    uint64_t physical_node_number = 1 + // meta data node
                                    // the '1' is for the mht node preceding every 96 data nodes
                                    mht_node_number * (1 + ATTACHED_DATA_NODES_COUNT);

    file_node_t* file_mht_node = (file_node_t*)lruc_find(pf->cache, physical_node_number);
    if (file_mht_node != NULL)
        return file_mht_node;

    file_node_t* parent_file_mht_node =
        ipf_read_mht_node(pf, (mht_node_number - 1) / CHILD_MHT_NODES_COUNT);

    if (parent_file_mht_node == NULL) // some error happened
        return NULL;

    file_mht_node = calloc(1, sizeof(*file_mht_node));
    if (!file_mht_node) {
        pf->last_error = PF_STATUS_NO_MEMORY;
        return NULL;
    }

    file_mht_node->type = FILE_MHT_NODE_TYPE;
    file_mht_node->node_number = mht_node_number;
    file_mht_node->physical_node_number = physical_node_number;
    file_mht_node->parent = parent_file_mht_node;

    if (!ipf_read_node(pf, pf->file, file_mht_node->physical_node_number,
                       file_mht_node->encrypted.cipher, PF_NODE_SIZE)) {
        free(file_mht_node);
        return NULL;
    }

    gcm_crypto_data_t* gcm_crypto_data = &file_mht_node->parent->decrypted.mht
        .mht_nodes_crypto[(file_mht_node->node_number - 1) % CHILD_MHT_NODES_COUNT];

    // this function decrypt the data _and_ checks the integrity of the data against the gmac
    status = cb_aes_gcm_decrypt(&gcm_crypto_data->key, &g_empty_iv,
                                NULL, 0,
                                file_mht_node->encrypted.cipher, PF_NODE_SIZE,
                                &file_mht_node->decrypted.mht,
                                &gcm_crypto_data->gmac);
    if (PF_FAILURE(status)) {
        free(file_mht_node);
        pf->last_error = status;
        if (status == PF_STATUS_MAC_MISMATCH)
            pf->file_status = PF_STATUS_CORRUPTED;
        return NULL;
    }

    if (!lruc_add(pf->cache, file_mht_node->physical_node_number, file_mht_node)) {
        erase_memory(&file_mht_node->decrypted, sizeof(file_mht_node->decrypted));
        free(file_mht_node);
        pf->last_error = PF_STATUS_NO_MEMORY;
        return NULL;
    }

    return file_mht_node;
}

static bool ipf_do_file_recovery(pf_context_t* pf, const char* path, uint32_t node_size) {
    pf_handle_t recovery_file = NULL;
    pf_handle_t source_file = NULL;
    uint32_t nodes_count = 0;
    uint32_t recovery_node_size = (uint32_t)(sizeof(uint64_t)) + node_size; // node offset + data
    uint64_t file_size = 0;
    uint8_t* recovery_node = NULL;
    uint32_t i = 0;
    bool ret = false;

    DEBUG_PF("pf %p, path '%s', recovery '%s'\n", pf, path, pf->recovery_path);

    if (!path || strnlen(path, 1) == 0 || !pf->recovery_path
        || strnlen(pf->recovery_path, 1) == 0) {

        pf->last_error = PF_STATUS_RECOVERY_IMPOSSIBLE;
        return false;
    }

    pf_status_t status = cb_open(pf->recovery_path, PF_FILE_MODE_READ, &recovery_file,
                                 &file_size);
    if (PF_FAILURE(status)) {
        pf->last_error = status;
        goto out;
    }

    if (file_size % recovery_node_size != 0) {
        // corrupted recovery file
        pf->last_error = PF_STATUS_CORRUPTED;
        goto out;
    }

    nodes_count = (uint32_t)(file_size / recovery_node_size);

    recovery_node = malloc(recovery_node_size);
    if (recovery_node == NULL) {
        pf->last_error = PF_STATUS_NO_MEMORY;
        goto out;
    }

    status = cb_open(path, PF_FILE_MODE_WRITE, &source_file, NULL);
    if (PF_FAILURE(status)) {
        pf->last_error = status;
        goto out;
    }

    for (i = 0; i < nodes_count; i++) {
        if (!ipf_read_node(pf, recovery_file, i, recovery_node, recovery_node_size)) {
            // last error already set
            goto out;
        }

        uint64_t node_number = *((uint64_t*)recovery_node);
        if (!ipf_write_node(pf, source_file, node_number, &recovery_node[sizeof(uint64_t)],
                            node_size)) {
            // last error already set
            goto out;
        }
    }

    if (i != nodes_count) // the 'for' loop exited with error
        goto out;

    ret = true;

out:
    free(recovery_node);
    if (source_file)
        cb_close(source_file);

    if (recovery_file)
        cb_close(recovery_file);

    if (ret)
        cb_delete(pf->recovery_path);

    return ret;
}

// public API

void pf_set_callbacks(pf_read_f read_f, pf_write_f write_f, pf_truncate_f truncate_f,
                      pf_open_f open_f, pf_close_f close_f,
                      pf_delete_f delete_f, pf_aes_gcm_encrypt_f aes_gcm_encrypt_f,
                      pf_aes_gcm_decrypt_f aes_gcm_decrypt_f, pf_random_f random_f,
                      pf_debug_f debug_f) {
    cb_read            = read_f;
    cb_write           = write_f;
    cb_truncate        = truncate_f;
    cb_open            = open_f;
    cb_close           = close_f;
    cb_delete          = delete_f;
    cb_aes_gcm_encrypt = aes_gcm_encrypt_f;
    cb_aes_gcm_decrypt = aes_gcm_decrypt_f;
    cb_random          = random_f;
    cb_debug           = debug_f;
    g_initialized      = true;
}

pf_status_t pf_open(pf_handle_t handle, const char* path, uint64_t underlying_size,
                    pf_file_mode_t mode, bool create, bool enable_recovery, const pf_key_t* key,
                    pf_context_t** context) {
    if (!g_initialized)
        return PF_STATUS_UNINITIALIZED;

    pf_status_t status;
    *context = ipf_open(path, mode, create, handle, underlying_size, key, enable_recovery, &status);
    return status;
}

pf_status_t pf_close(pf_context_t* pf) {
    if (!g_initialized)
        return PF_STATUS_UNINITIALIZED;

    if (ipf_close(pf))
        return PF_STATUS_SUCCESS;
    return pf->last_error;
}

pf_status_t pf_get_size(pf_context_t* pf, uint64_t* size) {
    if (!g_initialized)
        return PF_STATUS_UNINITIALIZED;

    *size = pf->encrypted_part_plain.size;
    return PF_STATUS_SUCCESS;
}

// TODO: file truncation
pf_status_t pf_set_size(pf_context_t* pf, uint64_t size) {
    if (!g_initialized)
        return PF_STATUS_UNINITIALIZED;

    if (!(pf->mode & PF_FILE_MODE_WRITE))
        return PF_STATUS_INVALID_MODE;

    if (size == pf->encrypted_part_plain.size)
        return PF_STATUS_SUCCESS;

    if (size > pf->encrypted_part_plain.size) {
        // extend the file
        pf->offset = pf->encrypted_part_plain.size;
        DEBUG_PF("extending the file from %lu to %lu\n", pf->offset, size);
        if (ipf_write(pf, NULL, size - pf->offset) != size - pf->offset)
            return pf->last_error;

        return PF_STATUS_SUCCESS;
    }

    return PF_STATUS_NOT_IMPLEMENTED;
}

pf_status_t pf_read(pf_context_t* pf, uint64_t offset, size_t size, void* output) {
    if (!g_initialized)
        return PF_STATUS_UNINITIALIZED;

    if (!ipf_seek(pf, offset))
        return pf->last_error;

    if (ipf_read(pf, output, size) != size)
        return pf->last_error;
    return PF_STATUS_SUCCESS;
}

pf_status_t pf_write(pf_context_t* pf, uint64_t offset, size_t size, const void* input) {
    if (!g_initialized)
        return PF_STATUS_UNINITIALIZED;

    if (!ipf_seek(pf, offset))
        return pf->last_error;

    if (ipf_write(pf, input, size) != size)
        return pf->last_error;
    return PF_STATUS_SUCCESS;
}

pf_status_t pf_flush(pf_context_t* pf) {
    if (!g_initialized)
        return PF_STATUS_UNINITIALIZED;

    if (!ipf_internal_flush(pf))
        return pf->last_error;
    return PF_STATUS_SUCCESS;
}

pf_status_t pf_get_handle(pf_context_t* pf, pf_handle_t* handle) {
    if (!g_initialized)
        return PF_STATUS_UNINITIALIZED;

    *handle = pf->file;
    return PF_STATUS_SUCCESS;
}
