// SPDX-License-Identifier: Apache-2.0
// Copyright 2019 Eotvos Lorand University, Budapest, Hungary

#include "dpdkx_crypto.h"

#if T4P4S_INIT_CRYPTO

#include <time.h>
#include <stdlib.h>
#include <rte_dev.h>
#include <rte_bus_vdev.h>
#include <rte_errno.h>

#ifdef RTE_LIBRTE_PMD_CRYPTO_SCHEDULER
    #include <rte_cryptodev_scheduler.h>
    #include <dataplane.h>
#endif

// -----------------------------------------------------------------------------
// Globals: memory pools, device and sessions

struct rte_mempool *session_pool, *session_priv_pool;
struct rte_mempool *crypto_pool;
struct rte_mempool *crypto_task_pool;

struct crypto_task *crypto_tasks[RTE_MAX_LCORE][CRYPTO_BURST_SIZE];
struct rte_crypto_op* enqueued_ops[RTE_MAX_LCORE][CRYPTO_BURST_SIZE];
struct rte_crypto_op* dequeued_ops[RTE_MAX_LCORE][CRYPTO_BURST_SIZE];


int cdev_id;

struct rte_cryptodev_sym_session *session_encrypt;
struct rte_cryptodev_sym_session *session_decrypt;
struct rte_cryptodev_sym_session *session_hmac;

uint8_t iv[16];
extern struct lcore_conf   lcore_conf[RTE_MAX_LCORE];

// -----------------------------------------------------------------------------
// Device initialisation and setup

static void setup_session(struct rte_cryptodev_sym_session **session, struct rte_mempool *session_pool)
{
    *session = rte_cryptodev_sym_session_create(session_pool);
    if (*session == NULL){
        rte_exit(EXIT_FAILURE, "Session could not be created\n");
    }
}

static void init_session(int cdev_id, struct rte_cryptodev_sym_session *session, struct rte_crypto_sym_xform *xform, struct rte_mempool *session_priv_pool)
{
    if (rte_cryptodev_sym_session_init(cdev_id, session, xform, session_priv_pool) < 0){
        rte_exit(EXIT_FAILURE, "Session could not be initialized for the crypto device\n");
    }
}

#define CRYPTO_MODE_OPENSSL 1
#define CRYPTO_MODE_NULL 2

#define CRYPTO_MODE CRYPTO_MODE_OPENSSL



static void setup_sessions()
{
    uint8_t cipher_key[] = {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0};

    struct rte_crypto_sym_xform cipher_xform_encrypt = DEFAULT_XFORM;
    #if CRYPTO_MODE == CRYPTO_MODE_OPENSSL
        cipher_xform_encrypt.cipher.op = RTE_CRYPTO_CIPHER_OP_ENCRYPT;
        cipher_xform_encrypt.cipher.key.data = cipher_key;
    #elif CRYPTO_MODE == CRYPTO_MODE_NULL
        cipher_xform_encrypt.cipher.algo = RTE_CRYPTO_CIPHER_NULL;
    #endif

    struct rte_crypto_sym_xform cipher_xform_decrypt = DEFAULT_XFORM;
    #if CRYPTO_MODE == CRYPTO_MODE_OPENSSL
        cipher_xform_decrypt.cipher.op = RTE_CRYPTO_CIPHER_OP_DECRYPT;
        cipher_xform_decrypt.cipher.key.data = cipher_key;
    #elif CRYPTO_MODE == CRYPTO_MODE_NULL
        cipher_xform_decrypt.cipher.algo = RTE_CRYPTO_CIPHER_NULL;
    #endif


    struct rte_crypto_sym_xform cipher_xform_hmac;
    cipher_xform_hmac.type = RTE_CRYPTO_SYM_XFORM_AUTH;
	cipher_xform_hmac.next = NULL;
	cipher_xform_hmac.auth.op = RTE_CRYPTO_AUTH_OP_GENERATE;

	cipher_xform_hmac.auth.algo = RTE_CRYPTO_AUTH_MD5_HMAC;

	cipher_xform_hmac.auth.digest_length = MD5_DIGEST_LEN;
	cipher_xform_hmac.auth.key.length = 16;
	uint8_t key_data[16] = {0x68,0x65,0x6c,0x6c,0x6f,0x74,0x34,0x70,0x34,0x73,0x75,0x73,0x65,0x72,0x7a,0x7a};
	cipher_xform_hmac.auth.key.data = key_data;


    setup_session(&session_encrypt, session_pool);
    setup_session(&session_decrypt, session_pool);
    setup_session(&session_hmac, session_pool);
    init_session(cdev_id, session_encrypt, &cipher_xform_encrypt, session_priv_pool);
    init_session(cdev_id, session_decrypt, &cipher_xform_decrypt, session_priv_pool);
    init_session(cdev_id, session_hmac, &cipher_xform_hmac, session_priv_pool);
}

static void crypto_init_storage(unsigned int session_size, uint8_t socket_id)
{
    session_pool = rte_cryptodev_sym_session_pool_create("session_pool", MAX_SESSIONS, session_size, POOL_CACHE_SIZE, 0, socket_id);
    session_priv_pool = session_pool;

    unsigned int crypto_op_private_data = AES_CBC_IV_LENGTH;
    crypto_pool = rte_crypto_op_pool_create("crypto_pool", RTE_CRYPTO_OP_TYPE_SYMMETRIC, 16*1024, POOL_CACHE_SIZE, crypto_op_private_data, socket_id);
    if (crypto_pool == NULL) rte_exit(EXIT_FAILURE, "Cannot create crypto op pool\n");

    crypto_task_pool = rte_mempool_create("crypto_task_pool", (unsigned)16*1024-1, sizeof(struct crypto_task), MEMPOOL_CACHE_SIZE, 0, NULL, NULL, NULL, NULL, 0, 0);
    if (crypto_task_pool == NULL) {
        switch(rte_errno){
            case E_RTE_NO_CONFIG:  rte_exit(EXIT_FAILURE, "Cannot create async op pool - function could not get pointer to rte_config structure\n"); break;
            case E_RTE_SECONDARY:  rte_exit(EXIT_FAILURE, "Cannot create async op pool - function was called from a secondary process instance\n"); break;
            case EINVAL:  rte_exit(EXIT_FAILURE, "Cannot create async op pool - cache size provided is too large\n"); break;
            case ENOSPC:  rte_exit(EXIT_FAILURE, "Cannot create async op pool - the maximum number of memzones has already been allocated\n"); break;
            case EEXIST:  rte_exit(EXIT_FAILURE, "Cannot create async op pool - a memzone with the same name already exists\n"); break;
            case ENOMEM:  rte_exit(EXIT_FAILURE, "Cannot create async op pool - no appropriate memory area found in which to create memzone\n"); break;
            default:  rte_exit(EXIT_FAILURE, "Cannot create async op pool - Unknown error %d\n",rte_errno); break;
        }
    }

}

static int setup_device(const char *crypto_name, uint8_t socket_id)
{
    int ret;
    char args[128];
    snprintf(args, sizeof(args), "socket_id=%d", socket_id);
    ret = rte_vdev_init(crypto_name, args);
    if (ret != 0)
        debug("Cannot create crypto device " T4LIT(%s,error) "\n", crypto_name);
    return rte_cryptodev_get_dev_id(crypto_name);
}

static void configure_device(int cdev_id, struct rte_cryptodev_config *conf, struct rte_cryptodev_qp_conf *qp_conf, uint8_t socket_id)
{
    if (rte_cryptodev_configure(cdev_id, conf) < 0)
        rte_exit(EXIT_FAILURE, "Failed to configure cryptodev %u", cdev_id);
    int i;
    for(i = 0; i < conf-> nb_queue_pairs; i++)
        if (rte_cryptodev_queue_pair_setup(cdev_id, i, qp_conf, socket_id) < 0)
            rte_exit(EXIT_FAILURE, "Failed to setup queue pair\n");
    if (rte_cryptodev_start(cdev_id) < 0)
        rte_exit(EXIT_FAILURE, "Failed to start device\n");
}



// Helper functions
void reset_pd(packet_descriptor_t *pd)
{
    pd->parsed_size = 0;
    if(pd->wrapper == 0){
        pd->payload_size = 0;
    }else{
        pd->payload_size = rte_pktmbuf_pkt_len(pd->wrapper) - pd->parsed_size;
    }
    pd->deparse_hdrinst_count = 0;
    pd->is_deparse_reordering = false;
}

void create_crypto_op(struct crypto_task **op_out, packet_descriptor_t* pd, enum crypto_task_type op_type, int offset, void* extraInformationForAsyncHandling){
    int ret = rte_mempool_get(crypto_task_pool, (void**)op_out);
    if(ret < 0){
        rte_exit(EXIT_FAILURE, "Mempool get failed!\n");
        //TODO: it should be a packet drop, not total fail
    }
    struct crypto_task *op = *op_out;
    op->op = op_type;
    op->data = pd->wrapper;

    op->plain_length = op->data->pkt_len - offset;
    op->offset = offset;
    debug_mbuf(op->data, " :::: Crypto: preparing packet");
    debug(" :::: Pkt length:%d\n",op->data->pkt_len);
    debug(" :::: Original offset:%d\n",offset);
    debug(" :::: Plain length:%d\n",op->plain_length);

    #if ASYNC_MODE == ASYNC_MODE_CONTEXT
        if(extraInformationForAsyncHandling != NULL){
            void* context = extraInformationForAsyncHandling;
            rte_pktmbuf_prepend(op->data, sizeof(void*));
            *(rte_pktmbuf_mtod(op->data, void**)) = context;
            op->offset += sizeof(void*);
        }
    #elif ASYNC_MODE == ASYNC_MODE_PD
        packet_descriptor_t *store_pd = extraInformationForAsyncHandling;
        *store_pd = *pd;

        rte_pktmbuf_prepend(op->data, sizeof(packet_descriptor_t*));
        *(rte_pktmbuf_mtod(op->data, packet_descriptor_t**)) = store_pd;
        op->offset += sizeof(void**);
    #endif

    rte_pktmbuf_prepend(op->data, sizeof(uint32_t));
    *(rte_pktmbuf_mtod(op->data, uint32_t*)) = op->plain_length;
    op->offset += sizeof(uint32_t);

    debug_mbuf(op->data, " :::: Added bytes for encryption");

    if(op->plain_length%16 != 0){
        op->padding_length = 16-op->plain_length%16;
        void* padding_memory = rte_pktmbuf_append(op->data, op->padding_length);
        memset(padding_memory,0,op->padding_length);
    }else{
        op->padding_length = 0;
    }

    debug(" :::: Padding size:%d\n",op->padding_length);
    debug(" :::: Offset :%d\n",op->offset);
    debug_mbuf(op->data, " :::: Final crypto task data:");
}

void crypto_task_to_crypto_op(struct crypto_task *crypto_task, struct rte_crypto_op *crypto_op)
{
    if(crypto_task->op == CRYPTO_TASK_MD5_HMAC){
        crypto_op->sym->auth.digest.data = (uint8_t *)rte_pktmbuf_append(crypto_task->data, MD5_DIGEST_LEN);
        crypto_op->sym->auth.data.offset = crypto_task->offset;
        crypto_op->sym->auth.data.length = crypto_task->plain_length;

        rte_crypto_op_attach_sym_session(crypto_op, session_hmac);
        crypto_op->sym->m_src = crypto_task->data;
    }else{
        crypto_op->sym->m_src = crypto_task->data;
        crypto_op->sym->cipher.data.offset = crypto_task->offset;
        crypto_op->sym->cipher.data.length = rte_pktmbuf_pkt_len(crypto_op->sym->m_src) - crypto_task->offset;
        debug("----------%d %d\n",crypto_op->sym->cipher.data.offset,crypto_op->sym->cipher.data.length);

        uint8_t *iv_ptr = rte_crypto_op_ctod_offset(crypto_op, uint8_t *, IV_OFFSET);
        memcpy(iv_ptr, iv, AES_CBC_IV_LENGTH);

        switch(crypto_task->op)
        {
        case CRYPTO_TASK_ENCRYPT:
            rte_crypto_op_attach_sym_session(crypto_op, session_encrypt);
            break;
        case CRYPTO_TASK_DECRYPT:
            rte_crypto_op_attach_sym_session(crypto_op, session_decrypt);
            break;
        }
    }
}


void do_blocking_sync_op(packet_descriptor_t* pd, enum crypto_task_type op, int offset){
    unsigned int lcore_id = rte_lcore_id();

    //control_DeparserImpl(pd, 0, 0);
    //emit_packet(pd, 0, 0);

    create_crypto_op(crypto_tasks[lcore_id],pd,op,offset,NULL);
    if (rte_crypto_op_bulk_alloc(crypto_pool, RTE_CRYPTO_OP_TYPE_SYMMETRIC, enqueued_ops[lcore_id], 1) == 0){
        rte_exit(EXIT_FAILURE, "Not enough crypto operations available\n");
    }
    crypto_task_to_crypto_op(crypto_tasks[lcore_id][0], enqueued_ops[lcore_id][0]);
    rte_mempool_put_bulk(crypto_task_pool, (void **) crypto_tasks[lcore_id], 1);

    #ifdef START_CRYPTO_NODE
        if (rte_ring_enqueue_burst(lcore_conf[lcore_id].fake_crypto_rx, (void**)enqueued_ops[lcore_id], 1, NULL) <= 0){
            debug(T4LIT(Enqueing ops in blocking sync op failed... skipping encryption,error) "\n");
            return;
        }
        while(rte_ring_dequeue_burst(lcore_conf[lcore_id].fake_crypto_tx, (void**)dequeued_ops[lcore_id], 1, NULL) == 0);
    #else
        if(rte_cryptodev_enqueue_burst(cdev_id, lcore_id,enqueued_ops[lcore_id], 1) <= 0){
            debug(T4LIT(Enqueing ops in blocking sync op failed... skipping encryption,error) "\n");
            return;
        }
        while(rte_cryptodev_dequeue_burst(cdev_id, lcore_id, dequeued_ops[lcore_id], 1) == 0);
    #endif
    if(op == CRYPTO_TASK_MD5_HMAC){
        //debug("%d\n",crypto_tasks[lcore_id][0]->padding_length);
        //debug_mbuf(crypto_tasks[lcore_id][0]->data,"FULL HMAC RESULT:");

        uint8_t *auth_tag;
        if (enqueued_ops[lcore_id][0]->sym->m_dst) {
            debug("m_dst not empty\n");
            auth_tag = rte_pktmbuf_mtod_offset(enqueued_ops[lcore_id][0]->sym->m_dst,uint8_t *,
                                               crypto_tasks[lcore_id][0]->padding_length);
            int target_position = crypto_tasks[lcore_id][0]->padding_length +
                               crypto_tasks[lcore_id][0]->plain_length +
                               crypto_tasks[lcore_id][0]->offset;
            uint8_t* target = rte_pktmbuf_mtod(pd->wrapper, uint8_t*) + target_position;
            memcpy(target,auth_tag,MD5_DIGEST_LEN);
        }
        rte_pktmbuf_adj(pd->wrapper, sizeof(int));
        /*dbg_bytes(  auth_tag, MD5_DIGEST_LEN,
              "HMAC RESULT (" T4LIT(%d) " bytes): ", MD5_DIGEST_LEN);*/
        /*memcpy(rte_pktmbuf_mtod(pd->wrapper, uint8_t*), auth_tag, MD5_DIGEST_LEN);
        pd->data = rte_pktmbuf_mtod(pd->wrapper, uint8_t*);
        pd->wrapper->pkt_len = MD5_DIGEST_LEN;
        pd->data = rte_pktmbuf_mtod(pd->wrapper, uint8_t*);*/
        debug_mbuf(pd->wrapper,"FULL HMAC RESULT:");
    }else{
        struct rte_mbuf *mbuf = dequeued_ops[lcore_id][0]->sym->m_src;
        int packet_size = *(rte_pktmbuf_mtod(mbuf, int*));

        rte_pktmbuf_adj(mbuf, sizeof(int));
        pd->wrapper = mbuf;
        pd->data = rte_pktmbuf_mtod(pd->wrapper, uint8_t*);
        pd->wrapper->pkt_len = packet_size;
        debug_mbuf(mbuf, "Result of encryption\n");
    }

    rte_mempool_put_bulk(crypto_pool, (void **)dequeued_ops[lcore_id], 1);
    reset_pd(pd);
    //parse_packet(pd, 0, 0);
}


// -----------------------------------------------------------------------------
// Callbacks

void init_crypto_devices()
{
    unsigned int session_size;
    uint8_t socket_id = rte_socket_id();

    #if CRYPTO_MODE == CRYPTO_MODE_OPENSSL
        cdev_id = setup_device("crypto_openssl0", socket_id);
    #elif CRYPTO_MODE == CRYPTO_MODE_NULL
        cdev_id = setup_device("crypto_null", socket_id);
    #endif

    if(CRYPTO_DEVICE_AVAILABLE)
    {
        session_size = rte_cryptodev_sym_get_private_session_size(cdev_id);
        crypto_init_storage(session_size, socket_id);
        struct rte_cryptodev_config conf = {
            .nb_queue_pairs = 8,
            .socket_id = socket_id
        };
        struct rte_cryptodev_qp_conf qp_conf = {
            .nb_descriptors = 2048,
            .mp_session = session_pool,
            .mp_session_private = session_priv_pool
        };
        configure_device(cdev_id, &conf, &qp_conf, socket_id);
        setup_sessions();
        srand(time(NULL));
        for(int i = 0; i < 16; i++) iv[i] = 0;//rand();
    }
    else
    {
        debug(T4LIT(Failed to setup crypto devices. Crypto operations are not available.,warning) "\n");
    }
}

// -----------------------------------------------------------------------------
// Implementation of P4 architecture externs

// defined in main_async.c
extern void do_crypto_task(packet_descriptor_t* pd, enum crypto_task_type op);
extern void do_encryption(SHORT_STDPARAMS);
extern void do_decryption(SHORT_STDPARAMS);

extern struct lcore_conf   lcore_conf[RTE_MAX_LCORE];

void do_encryption_async_impl(SHORT_STDPARAMS)
{
    #if ASYNC_MODE == ASYNC_MODE_CONTEXT
        if(pd->context != NULL){
            COUNTER_STEP(lcore_conf[rte_lcore_id()].doing_crypto_packet);
            do_crypto_task(pd, CRYPTO_TASK_ENCRYPT);
        }else{
            debug(T4LIT(Cannot find the context. We cannot do an async operation!,error) "\n");
            COUNTER_STEP(lcore_conf[rte_lcore_id()].fwd_packet);
        }
    #elif ASYNC_MODE == ASYNC_MODE_PD
        if(pd->context != NULL) {
            //debug("-----------------------------------------------Encrypt command, Program Phase: %d\n",pd->program_restore_phase)
            if(pd->program_restore_phase == 0){
                COUNTER_STEP(lcore_conf[rte_lcore_id()].doing_crypto_packet);
                do_crypto_task(pd, CRYPTO_TASK_ENCRYPT);
            }
        }else{
            COUNTER_STEP(lcore_conf[rte_lcore_id()].fwd_packet);
        }
    #elif ASYNC_MODE == ASYNC_MODE_SKIP
        COUNTER_STEP(lcore_conf[rte_lcore_id()].fwd_packet);
    #elif ASYNC_MODE == ASYNC_MODE_OFF
        do_encryption(SHORT_STDPARAMS_IN);
    #else
        #error Not Supported Async mode
    #endif
}

void do_decryption_async_impl(SHORT_STDPARAMS)
{
    #if ASYNC_MODE == ASYNC_MODE_CONTEXT
        if(pd->context != NULL) {
            do_crypto_task(pd, CRYPTO_TASK_DECRYPT);
        }else{
            debug(T4LIT(Cannot find the context. We cannot do an async operation!,error) "\n");
        }
    #elif ASYNC_MODE == ASYNC_MODE_PD
        if(pd->context != NULL) {
            //debug("-----------------------------------------------DECRYPT command, Program Phase: %d\n",pd->program_restore_phase)
            if(pd->program_restore_phase == 1){
                do_crypto_task(pd, CRYPTO_TASK_DECRYPT);
            }
        }
    #elif ASYNC_MODE == ASYNC_MODE_SKIP
        ;
    #elif ASYNC_MODE == ASYNC_MODE_OFF
        do_decryption(SHORT_STDPARAMS_IN);
    #else
        #error Not Supported Async mode
    #endif
}

void do_encryption(SHORT_STDPARAMS)
{
    #ifdef DEBUG__CRYPTO_EVERY_N
        if(lcore_conf[rte_lcore_id()].crypto_every_n_counter == 0){
            COUNTER_STEP(lcore_conf[rte_lcore_id()].doing_crypto_packet);
            COUNTER_STEP(lcore_conf[rte_lcore_id()].sent_to_crypto_packet);
            do_blocking_sync_op(pd, CRYPTO_TASK_ENCRYPT, 0);
        }else{
            COUNTER_STEP(lcore_conf[rte_lcore_id()].fwd_packet);
        }
    #else
        do_blocking_sync_op(pd, CRYPTO_TASK_ENCRYPT, 0);
    #endif
}

void do_decryption(SHORT_STDPARAMS)
{
    #ifdef DEBUG__CRYPTO_EVERY_N
        if(lcore_conf[rte_lcore_id()].crypto_every_n_counter == 0) {
            do_blocking_sync_op(pd, CRYPTO_TASK_DECRYPT, 0);
        }
        increase_with_rotation(lcore_conf[rte_lcore_id()].crypto_every_n_counter, DEBUG__CRYPTO_EVERY_N);
    #else
        do_blocking_sync_op(pd, CRYPTO_TASK_DECRYPT, 0);
    #endif
}

void do_ipsec_encapsulation(SHORT_STDPARAMS) {
    debug_mbuf(pd->wrapper,"START wrapper:");

    const int headers_size = 34;
    const int pad_length_size = 1;
    const int next_header_size = 1;
    const int esp_size = 8;
    const int iv_size = 8;

    int wrapper_size = rte_pktmbuf_pkt_len(pd->wrapper);
    int original_payload_size = wrapper_size - headers_size;


    int padding_size = (16 - (wrapper_size + pad_length_size + next_header_size)% 16) % 16;
    int to_encrypt_size = padding_size + wrapper_size + pad_length_size + next_header_size;

    debug(" :::: Padding length: %d\n",padding_size);
    debug(" :::: to_encrypt_size: %d\n",to_encrypt_size);

    rte_pktmbuf_append(pd->wrapper,esp_size + iv_size + to_encrypt_size - original_payload_size);
    uint8_t* data = rte_pktmbuf_mtod(pd->wrapper, uint8_t*);

    uint8_t* data_pointer = data;
    // Pass the existing header and leave space for the ESP and IV in payload (we forget about the original payload)
    data_pointer += headers_size + esp_size + iv_size;
    // Important to use memmove, because the source and destination can overlap
    memmove(data_pointer, data, wrapper_size);

    // Run to the end of the copied header+payload
    data_pointer += wrapper_size;
    // And set the padding bytes
    memset(data_pointer, 0xef, padding_size);

    // Save the size of padding
    data_pointer += padding_size;
    memset(data_pointer, (uint8_t)padding_size, 1);

    // Set next header to ipv4
    data_pointer += 1;
    memset(data_pointer, 4, 1);

    // TODO: add ESP
    // ESP
    memset(data + headers_size, 0x1a, 8);
    // TODO: use random IV
    // IV
    memset(data + headers_size + esp_size, 0x1b, 8);

    do_blocking_sync_op(pd, CRYPTO_TASK_ENCRYPT, headers_size + esp_size + iv_size);

    do_blocking_sync_op(pd, CRYPTO_TASK_MD5_HMAC, headers_size);

    // We keep only 12 bytes from 16 byte HMAC
    rte_pktmbuf_trim(pd->wrapper,4);

    debug_mbuf(pd->wrapper,"final wrapper");
}

void md5_hmac_impl(uint8_buffer_t offset, SHORT_STDPARAMS)
{
    do_blocking_sync_op(pd, CRYPTO_TASK_MD5_HMAC, offset.buffer[0]);
}

void encrypt__u8s(uint8_buffer_t offset, SHORT_STDPARAMS)
{
    do_blocking_sync_op(pd, CRYPTO_TASK_ENCRYPT, offset.buffer[0]);
}

#endif
