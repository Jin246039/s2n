/*
 * Copyright 2017 Amazon.com, Inc. or its affiliates. All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License").
 * You may not use this file except in compliance with the License.
 * A copy of the License is located at
 *
 *  http://aws.amazon.com/apache2.0
 *
 * or in the "license" file accompanying this file. This file is distributed
 * on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either
 * express or implied. See the License for the specific language governing
 * permissions and limitations under the License.
 */

#include "s2n_test.h"
#include <s2n.h>

#include <stdlib.h>

#include "testlib/s2n_testlib.h"

#include "tls/s2n_connection.h"
#include "tls/s2n_handshake.h"
#include "tls/s2n_cipher_preferences.h"
#include "tls/s2n_cipher_suites.h"
#include "utils/s2n_safety.h"

int buffer_read(void *io_context, uint8_t *buf, uint32_t len)
{
    struct s2n_stuffer *in_buf;
    int n_read, n_avail;

    if (buf == NULL) {
        return 0;
    }

    in_buf = (struct s2n_stuffer *) io_context;
    if (in_buf == NULL) {
        errno = EINVAL;
        return -1;
    }

    // read the number of bytes requested or less if it isn't available
    n_avail = s2n_stuffer_data_available(in_buf);
    n_read = (len < n_avail) ? len : n_avail;

    if (n_read == 0) {
        errno = EAGAIN;
        return -1;
    }

    s2n_stuffer_read_bytes(in_buf, buf, n_read);
    return n_read;
}

int buffer_write(void *io_context, const uint8_t *buf, uint32_t len)
{
    struct s2n_stuffer *out;

    if (buf == NULL) {
        return 0;
    }

    out = (struct s2n_stuffer *) io_context;
    if (out == NULL) {
        errno = EINVAL;
        return -1;
    }

    if (s2n_stuffer_write_bytes(out, buf, len) < 0) {
        errno = EAGAIN;
        return -1;
    }

    return len;
}

extern message_type_t s2n_conn_get_current_message_type(struct s2n_connection *conn);

static const int MAX_TRIES = 100;

int main(int argc, char **argv)
{
    struct s2n_config *config;
    const struct s2n_cipher_preferences *default_cipher_preferences;
    char *cert_chain_pem;
    char *private_key_pem;
    char *dhparams_pem;

    BEGIN_TEST();
    EXPECT_SUCCESS(setenv("S2N_ENABLE_CLIENT_MODE", "1", 0));
    EXPECT_NOT_NULL(cert_chain_pem = malloc(S2N_MAX_TEST_PEM_SIZE));
    EXPECT_NOT_NULL(private_key_pem = malloc(S2N_MAX_TEST_PEM_SIZE));
    EXPECT_NOT_NULL(dhparams_pem = malloc(S2N_MAX_TEST_PEM_SIZE));

    /*
     * Test Mutual Auth using **s2n_connection_set_client_auth_type**
     */

    EXPECT_NOT_NULL(config = s2n_config_new());
    EXPECT_SUCCESS(s2n_read_test_pem(S2N_DEFAULT_TEST_CERT_CHAIN, cert_chain_pem, S2N_MAX_TEST_PEM_SIZE));
    EXPECT_SUCCESS(s2n_read_test_pem(S2N_DEFAULT_TEST_PRIVATE_KEY, private_key_pem, S2N_MAX_TEST_PEM_SIZE));
    EXPECT_SUCCESS(s2n_read_test_pem(S2N_DEFAULT_TEST_DHPARAMS, dhparams_pem, S2N_MAX_TEST_PEM_SIZE));
    EXPECT_SUCCESS(s2n_config_add_cert_chain_and_key(config, cert_chain_pem, private_key_pem));
    EXPECT_SUCCESS(s2n_config_add_dhparams(config, dhparams_pem));
    EXPECT_NOT_NULL(default_cipher_preferences = config->cipher_preferences);

    EXPECT_SUCCESS(s2n_config_set_verify_cert_chain_cb(config, &accept_all_rsa_certs, NULL));

    /* Verify that a handshake succeeds for every cipher in the default list. */
    for (int cipher_idx = 0; cipher_idx < default_cipher_preferences->count; cipher_idx++) {
        struct s2n_cipher_preferences server_cipher_preferences;
        struct s2n_connection *client_conn;
        struct s2n_connection *server_conn;
        s2n_blocked_status client_blocked;
        s2n_blocked_status server_blocked;
        struct s2n_stuffer client_to_server;
        struct s2n_stuffer server_to_client;

        /* Craft a cipher preference with a cipher_idx cipher
           NOTE: Its safe to use memcpy as the address of server_cipher_preferences
           will never be NULL */
        memcpy(&server_cipher_preferences, default_cipher_preferences, sizeof(server_cipher_preferences));
        server_cipher_preferences.count = 1;
        struct s2n_cipher_suite *cur_cipher = default_cipher_preferences->suites[cipher_idx];

        if (!cur_cipher->available) {
            /* Skip Ciphers that aren't supported with the linked libcrypto */
            continue;
        }

        server_cipher_preferences.suites = &cur_cipher;
        config->cipher_preferences = &server_cipher_preferences;

        EXPECT_NOT_NULL(server_conn = s2n_connection_new(S2N_SERVER));
        EXPECT_SUCCESS(s2n_connection_set_config(server_conn, config));

        EXPECT_NOT_NULL(client_conn = s2n_connection_new(S2N_CLIENT));
        EXPECT_SUCCESS(s2n_connection_set_config(client_conn, config));

        EXPECT_SUCCESS(s2n_connection_set_client_auth_type(server_conn, S2N_CERT_AUTH_REQUIRED));
        EXPECT_SUCCESS(s2n_connection_set_client_auth_type(client_conn, S2N_CERT_AUTH_REQUIRED));

        /* Set up our I/O callbacks. Use stuffers for the "I/O context" */
        EXPECT_SUCCESS(s2n_stuffer_growable_alloc(&client_to_server, 0));
        EXPECT_SUCCESS(s2n_stuffer_growable_alloc(&server_to_client, 0));

        /* Set Up Callbacks*/
        EXPECT_SUCCESS(s2n_connection_set_recv_cb(client_conn, &buffer_read));
        EXPECT_SUCCESS(s2n_connection_set_send_cb(client_conn, &buffer_write));
        EXPECT_SUCCESS(s2n_connection_set_recv_cb(server_conn, &buffer_read));
        EXPECT_SUCCESS(s2n_connection_set_send_cb(server_conn, &buffer_write));

        /* Set up Callback Contexts to use stuffers */
        EXPECT_SUCCESS(s2n_connection_set_recv_ctx(client_conn, &server_to_client));
        EXPECT_SUCCESS(s2n_connection_set_send_ctx(client_conn, &client_to_server));
        EXPECT_SUCCESS(s2n_connection_set_recv_ctx(server_conn, &client_to_server));
        EXPECT_SUCCESS(s2n_connection_set_send_ctx(server_conn, &server_to_client));

        int tries = 0;
        do {
            int ret;
            ret = s2n_negotiate(client_conn, &client_blocked);
            EXPECT_TRUE(ret == 0 || (client_blocked && errno == EAGAIN));
            ret = s2n_negotiate(server_conn, &server_blocked);
            EXPECT_TRUE(ret == 0 || (server_blocked && errno == EAGAIN));
            tries += 1;

            if (tries >= MAX_TRIES) {
               FAIL();
            }
        } while (client_blocked || server_blocked);

        /* Verify that both connections negotiated Mutual Auth */
        EXPECT_TRUE(s2n_connection_client_cert_used(server_conn));
        EXPECT_TRUE(s2n_connection_client_cert_used(client_conn));

        EXPECT_SUCCESS(s2n_connection_free(client_conn));
        EXPECT_SUCCESS(s2n_connection_free(server_conn));
    }


    /*
     * Test Mutual Auth using **s2n_config_set_client_auth_type**
     */

    EXPECT_SUCCESS(s2n_config_set_client_auth_type(config, S2N_CERT_AUTH_REQUIRED));

    /* Verify that a handshake succeeds for every cipher in the default list. */
    for (int cipher_idx = 0; cipher_idx < default_cipher_preferences->count; cipher_idx++) {
        struct s2n_cipher_preferences server_cipher_preferences;
        struct s2n_connection *client_conn;
        struct s2n_connection *server_conn;
        s2n_blocked_status client_blocked;
        s2n_blocked_status server_blocked;
        struct s2n_stuffer client_to_server;
        struct s2n_stuffer server_to_client;

        /* Craft a cipher preference with a cipher_idx cipher
           NOTE: Its safe to use memcpy as the address of server_cipher_preferences
           will never be NULL */
        memcpy(&server_cipher_preferences, default_cipher_preferences, sizeof(server_cipher_preferences));
        server_cipher_preferences.count = 1;
        struct s2n_cipher_suite *cur_cipher = default_cipher_preferences->suites[cipher_idx];

        if (!cur_cipher->available) {
            /* Skip Ciphers that aren't supported with the linked libcrypto */
            continue;
        }

        server_cipher_preferences.suites = &cur_cipher;
        config->cipher_preferences = &server_cipher_preferences;

        EXPECT_NOT_NULL(server_conn = s2n_connection_new(S2N_SERVER));
        EXPECT_SUCCESS(s2n_connection_set_config(server_conn, config));

        EXPECT_NOT_NULL(client_conn = s2n_connection_new(S2N_CLIENT));
        EXPECT_SUCCESS(s2n_connection_set_config(client_conn, config));

        /* Set up our I/O callbacks. Use stuffers for the "I/O context" */
        EXPECT_SUCCESS(s2n_stuffer_growable_alloc(&client_to_server, 0));
        EXPECT_SUCCESS(s2n_stuffer_growable_alloc(&server_to_client, 0));

        /* Set Up Callbacks*/
        EXPECT_SUCCESS(s2n_connection_set_recv_cb(client_conn, &buffer_read));
        EXPECT_SUCCESS(s2n_connection_set_send_cb(client_conn, &buffer_write));
        EXPECT_SUCCESS(s2n_connection_set_recv_cb(server_conn, &buffer_read));
        EXPECT_SUCCESS(s2n_connection_set_send_cb(server_conn, &buffer_write));

        /* Set up Callback Contexts to use stuffers */
        EXPECT_SUCCESS(s2n_connection_set_recv_ctx(client_conn, &server_to_client));
        EXPECT_SUCCESS(s2n_connection_set_send_ctx(client_conn, &client_to_server));
        EXPECT_SUCCESS(s2n_connection_set_recv_ctx(server_conn, &client_to_server));
        EXPECT_SUCCESS(s2n_connection_set_send_ctx(server_conn, &server_to_client));

        int tries = 0;
        do {
            int ret;
            ret = s2n_negotiate(client_conn, &client_blocked);
            EXPECT_TRUE(ret == 0 || (client_blocked && errno == EAGAIN));
            ret = s2n_negotiate(server_conn, &server_blocked);
            EXPECT_TRUE(ret == 0 || (server_blocked && errno == EAGAIN));
            tries += 1;

            if (tries >= MAX_TRIES) {
               FAIL();
            }
        } while (client_blocked || server_blocked);

        /* Verify that both connections negotiated Mutual Auth */
        EXPECT_TRUE(s2n_connection_client_cert_used(server_conn));
        EXPECT_TRUE(s2n_connection_client_cert_used(client_conn));

        EXPECT_SUCCESS(s2n_connection_free(client_conn));
        EXPECT_SUCCESS(s2n_connection_free(server_conn));
    }


    /*
     * Test Mutual Auth using connection override of **s2n_config_set_client_auth_type**
     */

    EXPECT_SUCCESS(s2n_config_set_client_auth_type(config, S2N_CERT_AUTH_NONE));

    /* Verify that a handshake succeeds for every cipher in the default list. */
    for (int cipher_idx = 0; cipher_idx < default_cipher_preferences->count; cipher_idx++) {
        struct s2n_cipher_preferences server_cipher_preferences;
        struct s2n_connection *client_conn;
        struct s2n_connection *server_conn;
        s2n_blocked_status client_blocked;
        s2n_blocked_status server_blocked;
        struct s2n_stuffer client_to_server;
        struct s2n_stuffer server_to_client;

        /* Craft a cipher preference with a cipher_idx cipher
           NOTE: Its safe to use memcpy as the address of server_cipher_preferences
           will never be NULL */
        memcpy(&server_cipher_preferences, default_cipher_preferences, sizeof(server_cipher_preferences));
        server_cipher_preferences.count = 1;
        struct s2n_cipher_suite *cur_cipher = default_cipher_preferences->suites[cipher_idx];

        if (!cur_cipher->available) {
            /* Skip Ciphers that aren't supported with the linked libcrypto */
            continue;
        }

        server_cipher_preferences.suites = &cur_cipher;
        config->cipher_preferences = &server_cipher_preferences;

        EXPECT_NOT_NULL(server_conn = s2n_connection_new(S2N_SERVER));
        EXPECT_SUCCESS(s2n_connection_set_config(server_conn, config));

        EXPECT_NOT_NULL(client_conn = s2n_connection_new(S2N_CLIENT));
        EXPECT_SUCCESS(s2n_connection_set_config(client_conn, config));

        EXPECT_SUCCESS(s2n_connection_set_client_auth_type(server_conn, S2N_CERT_AUTH_REQUIRED));
        EXPECT_SUCCESS(s2n_connection_set_client_auth_type(client_conn, S2N_CERT_AUTH_REQUIRED));

        /* Set up our I/O callbacks. Use stuffers for the "I/O context" */
        EXPECT_SUCCESS(s2n_stuffer_growable_alloc(&client_to_server, 0));
        EXPECT_SUCCESS(s2n_stuffer_growable_alloc(&server_to_client, 0));

        /* Set Up Callbacks*/
        EXPECT_SUCCESS(s2n_connection_set_recv_cb(client_conn, &buffer_read));
        EXPECT_SUCCESS(s2n_connection_set_send_cb(client_conn, &buffer_write));
        EXPECT_SUCCESS(s2n_connection_set_recv_cb(server_conn, &buffer_read));
        EXPECT_SUCCESS(s2n_connection_set_send_cb(server_conn, &buffer_write));

        /* Set up Callback Contexts to use stuffers */
        EXPECT_SUCCESS(s2n_connection_set_recv_ctx(client_conn, &server_to_client));
        EXPECT_SUCCESS(s2n_connection_set_send_ctx(client_conn, &client_to_server));
        EXPECT_SUCCESS(s2n_connection_set_recv_ctx(server_conn, &client_to_server));
        EXPECT_SUCCESS(s2n_connection_set_send_ctx(server_conn, &server_to_client));

        int tries = 0;
        do {
            int ret;
            ret = s2n_negotiate(client_conn, &client_blocked);
            EXPECT_TRUE(ret == 0 || (client_blocked && errno == EAGAIN));
            ret = s2n_negotiate(server_conn, &server_blocked);
            EXPECT_TRUE(ret == 0 || (server_blocked && errno == EAGAIN));
            tries += 1;

            if (tries >= MAX_TRIES) {
               FAIL();
            }
        } while (client_blocked || server_blocked);

        /* Verify that both connections negotiated Mutual Auth */
        EXPECT_TRUE(s2n_connection_client_cert_used(server_conn));
        EXPECT_TRUE(s2n_connection_client_cert_used(client_conn));

        EXPECT_SUCCESS(s2n_connection_free(client_conn));
        EXPECT_SUCCESS(s2n_connection_free(server_conn));
    }

    /*
     * Test Mutual Auth using connection override of **s2n_config_set_client_auth_type** only on one side of the
     * connection and verify that a connection is not established
     */

    EXPECT_SUCCESS(s2n_config_set_client_auth_type(config, S2N_CERT_AUTH_NONE));

    /* Verify that a handshake succeeds for every cipher in the default list. */
    for (int cipher_idx = 0; cipher_idx < default_cipher_preferences->count; cipher_idx++) {
        struct s2n_cipher_preferences server_cipher_preferences;
        struct s2n_connection *client_conn;
        struct s2n_connection *server_conn;
        s2n_blocked_status client_blocked;
        s2n_blocked_status server_blocked;
        struct s2n_stuffer client_to_server;
        struct s2n_stuffer server_to_client;

        /* Craft a cipher preference with a cipher_idx cipher
           NOTE: Its safe to use memcpy as the address of server_cipher_preferences
           will never be NULL */
        memcpy(&server_cipher_preferences, default_cipher_preferences, sizeof(server_cipher_preferences));
        server_cipher_preferences.count = 1;
        struct s2n_cipher_suite *cur_cipher = default_cipher_preferences->suites[cipher_idx];

        if (!cur_cipher->available) {
            /* Skip Ciphers that aren't supported with the linked libcrypto */
            continue;
        }

        server_cipher_preferences.suites = &cur_cipher;
        config->cipher_preferences = &server_cipher_preferences;

        EXPECT_NOT_NULL(server_conn = s2n_connection_new(S2N_SERVER));
        EXPECT_SUCCESS(s2n_connection_set_config(server_conn, config));

        EXPECT_NOT_NULL(client_conn = s2n_connection_new(S2N_CLIENT));
        EXPECT_SUCCESS(s2n_connection_set_config(client_conn, config));

        /* Only set S2N_CERT_AUTH_REQUIRED on the server and not the client so that the connection fails */
        EXPECT_SUCCESS(s2n_connection_set_client_auth_type(server_conn, S2N_CERT_AUTH_REQUIRED));


        /* Set up our I/O callbacks. Use stuffers for the "I/O context" */
        EXPECT_SUCCESS(s2n_stuffer_growable_alloc(&client_to_server, 0));
        EXPECT_SUCCESS(s2n_stuffer_growable_alloc(&server_to_client, 0));

        /* Set Up Callbacks*/
        EXPECT_SUCCESS(s2n_connection_set_recv_cb(client_conn, &buffer_read));
        EXPECT_SUCCESS(s2n_connection_set_send_cb(client_conn, &buffer_write));
        EXPECT_SUCCESS(s2n_connection_set_recv_cb(server_conn, &buffer_read));
        EXPECT_SUCCESS(s2n_connection_set_send_cb(server_conn, &buffer_write));

        /* Set up Callback Contexts to use stuffers */
        EXPECT_SUCCESS(s2n_connection_set_recv_ctx(client_conn, &server_to_client));
        EXPECT_SUCCESS(s2n_connection_set_send_ctx(client_conn, &client_to_server));
        EXPECT_SUCCESS(s2n_connection_set_recv_ctx(server_conn, &client_to_server));
        EXPECT_SUCCESS(s2n_connection_set_send_ctx(server_conn, &server_to_client));

        int tries = 0;
        int failures = 0;
        do {
            int client_ret, server_ret;
            client_ret = s2n_negotiate(client_conn, &client_blocked);
            server_ret = s2n_negotiate(server_conn, &server_blocked);
            tries += 1;

            if (client_ret != 0 || server_ret != 0) {
               failures ++;
            }
        } while ((client_blocked || server_blocked) && tries < MAX_TRIES);

        EXPECT_EQUAL(failures, MAX_TRIES);
        /* Verify that NEITHER connections negotiated Mutual Auth */
        EXPECT_FALSE(s2n_connection_client_cert_used(server_conn));
        EXPECT_FALSE(s2n_connection_client_cert_used(client_conn));

        EXPECT_SUCCESS(s2n_connection_free(client_conn));
        EXPECT_SUCCESS(s2n_connection_free(server_conn));
    }

    EXPECT_SUCCESS(s2n_config_free(config));
    free(cert_chain_pem);
    free(private_key_pem);
    free(dhparams_pem);
    END_TEST();
    return 0;
}
