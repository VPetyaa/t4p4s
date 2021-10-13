// SPDX-License-Identifier: Apache-2.0
// Copyright 2020 Eotvos Lorand University, Budapest, Hungary

#include "gen_include.h"
#include "main.h"

#include <rte_ethdev.h>
#include <rte_mempool.h>
#include <rte_spinlock.h>

extern bool is_packet_dropped(packet_descriptor_t* pd);
static rte_spinlock_t spinlock = {RTE_SPINLOCK_INITIALIZER};

volatile int packet_counter = 0;
volatile int packet_with_error_counter = 0;


static uint32_t qlatency = 0;
static int qdepth = 0;
static int qdepth_bytes = 0;
static int maxqsbyte = 0;
static int avg_qdepth = 0;
static int avg_qdepth_bytes = 0;
static int timestamp = 0;
static int queue_drop = 0;
static uint32_t last_t = 0;
static int ingress_drop = 0;
static int rx_rate = 0;

#define WQ 0.002
#define PD_POOL_SIZE 1024*8

static inline void
wait_for_cycles(uint64_t cycles)
{
        uint64_t now = rte_get_tsc_cycles();
        uint64_t then = now;
        while((now - then) < cycles)
                now = rte_get_tsc_cycles();
}


void get_broadcast_port_msg(char result[256], int ingress_port) {
    uint8_t nb_ports = get_port_count();
    uint32_t port_mask = get_port_mask();

    char* result_ptr = result;
    bool is_first_printed_port = true;
    for (uint8_t portidx = 0; portidx < RTE_MAX_ETHPORTS; ++portidx) {
        if (portidx == ingress_port) {
           continue;
        }

        bool is_port_disabled = (port_mask & (1 << portidx)) == 0;
        if (is_port_disabled)   continue;

        int printed_bytes = sprintf(result_ptr, "%s" T4LIT(%d,port), is_first_printed_port ? "" : ", ", portidx);
        result_ptr += printed_bytes;
        is_first_printed_port = false;
    }
}


void broadcast_packet(int egress_port, int ingress_port, LCPARAMS)
{
    uint8_t nb_ports = get_port_count();
    uint32_t port_mask = get_port_mask();

    uint8_t nb_port = 0;
    int broadcast_idx = 0;
    for (uint8_t portidx = 0; nb_port < nb_ports - 1 && portidx < RTE_MAX_ETHPORTS; ++portidx) {
        if (portidx == ingress_port) {
           continue;
        }

        bool is_port_disabled = (port_mask & (1 << portidx)) == 0;
        if (is_port_disabled)   continue;

        packet* pkt_out = (nb_port < nb_ports) ? clone_packet(pd->wrapper, lcdata->mempool) : pd->wrapper;
        send_single_packet(pkt_out, egress_port, ingress_port, broadcast_idx != 0, LCPARAMS_IN);

        ++nb_port;
        ++broadcast_idx;
    }

    if (unlikely(nb_port != nb_ports - 1)) {
        debug(" " T4LIT(!!!!,error) " " T4LIT(Wrong port count,error) ": " T4LIT(%d) " ports should be present, but only " T4LIT(%d) " found\n", nb_ports, nb_port);
    }
}

/* Enqueue a single packet, and send burst if queue is filled */
void send_packet(int egress_port, int ingress_port, LCPARAMS)
{
    uint32_t lcore_id = rte_lcore_id();
    struct rte_mbuf* mbuf = (struct rte_mbuf *)pd->wrapper;

    if (unlikely(egress_port == T4P4S_BROADCAST_PORT)) {
        #ifdef T4P4S_DEBUG
            char ports_msg[256];
            get_broadcast_port_msg(ports_msg, ingress_port);
            debug(" " T4LIT(<<<<,outgoing) " " T4LIT(Broadcasting,outgoing) " packet " T4LIT(#%03d) "%s from port " T4LIT(%d,port) " to all other ports (%s): " T4LIT(%dB) " of headers, " T4LIT(%dB) " of payload\n",
                  get_packet_idx(LCPARAMS_IN),  pd->is_deparse_reordering ? "" : " with " T4LIT(unchanged structure),
                  ingress_port, ports_msg, rte_pktmbuf_pkt_len(mbuf) - pd->payload_size, pd->payload_size);
        #endif
        broadcast_packet(egress_port, ingress_port, LCPARAMS_IN);
    } else {
        debug(" " T4LIT(<<<<,outgoing) " " T4LIT(Emitting,outgoing) " packet " T4LIT(#%03d) "%s on port " T4LIT(%d,port) ": " T4LIT(%dB) " of headers, " T4LIT(%dB) " of payload\n",
              get_packet_idx(LCPARAMS_IN),  pd->is_deparse_reordering ? "" : " with " T4LIT(unchanged structure),
              egress_port, rte_pktmbuf_pkt_len(mbuf) - pd->payload_size, pd->payload_size);
        send_single_packet(pd->wrapper, egress_port, ingress_port, false, LCPARAMS_IN);
    }
}

void do_single_tx(LCPARAMS)
{
    if (unlikely(is_packet_dropped(pd))) {
        free_packet(LCPARAMS_IN);
    } else {
        int egress_port = get_egress_port(pd);
        int ingress_port = get_ingress_port(pd);

        send_packet(egress_port, ingress_port, LCPARAMS_IN);
    }
}

void do_handle_packet(int portid, unsigned queue_idx, unsigned pkt_idx, LCPARAMS)
{
    struct lcore_state state = lcdata->conf->state;
    lookup_table_t** tables = state.tables;
    parser_state_t* pstate = &(state.parser_state);
    init_parser_state(&(state.parser_state));

    handle_packet(portid, get_packet_idx(LCPARAMS_IN), STDPARAMS_IN);
    do_single_tx(LCPARAMS_IN);

    #if ASYNC_MODE == ASYNC_MODE_CONTEXT
        if (pd->context != NULL) {
            debug(" " T4LIT(<<<<,async) " Context for packet " T4LIT(%p,bytes) " terminating, swapping back to " T4LIT(main context,async) "\n", pd->context);
            rte_ring_enqueue(context_free_command_ring, pd->context);
        }
    #endif
}

// defined in main_async.c
void async_handle_packet(int port_id, unsigned queue_idx, unsigned pkt_idx, packet_handler_t handler_function, LCPARAMS);
void main_loop_async(LCPARAMS);
void main_loop_fake_crypto(LCPARAMS);

void init_pd_state(packet_descriptor_t* pd) {
    pd->context = NULL;
    pd->program_restore_phase = 0;
}

// TODO move this to stats.h.py
extern void print_async_stats(LCPARAMS);

void do_single_rx(unsigned queue_idx, unsigned pkt_idx, LCPARAMS)
{
    print_async_stats(LCPARAMS_IN);

    init_pd_state(pd);

    bool got_packet = receive_packet(pkt_idx, LCPARAMS_IN);
    if (likely(got_packet && is_packet_handled(LCPARAMS_IN))) {
        int portid = get_portid(queue_idx, LCPARAMS_IN);
        #if ASYNC_MODE == ASYNC_MODE_CONTEXT || ASYNC_MODE == ASYNC_MODE_PD
            if (unlikely(PACKET_REQUIRES_ASYNC(lcdata,pd))) {
                COUNTER_STEP(lcdata->conf->sent_to_crypto_packet);
                async_handle_packet(portid, queue_idx, pkt_idx, do_handle_packet, LCPARAMS_IN);
                return;
            }
        #endif

        do_handle_packet(portid, queue_idx, pkt_idx, LCPARAMS_IN);
    }

    main_loop_post_single_rx(got_packet, LCPARAMS_IN);
}

bool do_rx(LCPARAMS)
{
    bool got_packet = false;
    unsigned queue_count = get_queue_count(lcdata);
    for (unsigned queue_idx = 0; queue_idx < queue_count; queue_idx++) {
        main_loop_rx_group(queue_idx, LCPARAMS_IN);

        unsigned pkt_count = get_pkt_count_in_group(lcdata);
        got_packet |= pkt_count > 0;
        for (unsigned pkt_idx = 0; pkt_idx < pkt_count; pkt_idx++) {
            do_single_rx(queue_idx, pkt_idx, LCPARAMS_IN);
        }
    }

    return got_packet;
}

void main_loop_pre_do_post_rx(LCPARAMS){
    main_loop_pre_rx(LCPARAMS_IN);
    bool got_packet = do_rx(LCPARAMS_IN);
    main_loop_post_rx(got_packet, LCPARAMS_IN);
}

int crypto_node_id() {
    return rte_lcore_count() - 1;
}

bool is_crypto_node() {
    return rte_lcore_id() == crypto_node_id();
}

bool initial_check(LCPARAMS) {
    if (!lcdata->is_valid) {
        debug("lcore data is invalid, exiting\n");
        #ifdef START_CRYPTO_NODE
            if (!is_crypto_node()) return false;
        #else
            return false;
        #endif
    }

    #ifdef START_CRYPTO_NODE
        if (is_crypto_node()){
            RTE_LOG(INFO, P4_FWD, "lcore %u is the crypto node\n", lcore_id);
        }
    #endif

    return true;
}

void init_stats(LCPARAMS)
{
    COUNTER_INIT(lcdata->conf->processed_packet_num);
    COUNTER_INIT(lcdata->conf->async_packet);
    COUNTER_INIT(lcdata->conf->sent_to_crypto_packet);
    COUNTER_INIT(lcdata->conf->doing_crypto_packet);
    COUNTER_INIT(lcdata->conf->fwd_packet);
}


void do_ring_egressing(LCPARAMS)
{
    packet_descriptor_t* tmp[32];
    struct rte_mbuf* mtmp[1];
    int ret, nb_rx, i;
    int pkt_len;
    uint32_t value32;
    int egress_port;
    static int tbsize = 1000;
    static uint32_t last = 0;

    rte_spinlock_lock(&spinlock);
    nb_rx = rte_ring_sc_dequeue_burst( lcdata->conf->txring->ring,
                                (void **)mtmp, 1, NULL);
    if (nb_rx==1) qdepth_bytes -= mtmp[0]->pkt_len;
    rte_spinlock_unlock(&spinlock);
    
    if (nb_rx==0) {
        rte_pause();
        return;
    }
    pkt_len = mtmp[0]->pkt_len;
    send_single_packet(mtmp[0], 1, 0, false, LCPARAMS_IN);
    
    wait_for_cycles((pkt_len << 8) / 45);
}


void do_simple_forward_for_other_port_traffic(LCPARAMS) {

    unsigned queue_count = get_queue_count(lcdata);
    for (unsigned queue_idx = 0; queue_idx < queue_count; queue_idx++) {
        main_loop_rx_group(queue_idx, LCPARAMS_IN);
        unsigned pkt_count = get_pkt_count_in_group(lcdata);
        for (unsigned pkt_idx = 0; pkt_idx < pkt_count; pkt_idx++) {
                send_single_packet(lcdata->pkts_burst[pkt_idx], 0, 1, false, LCPARAMS_IN);
        }
    }
}

int process_pkt(struct rte_mbuf* m, struct lcore_data* lcdata,  STDPARAMS)
{
        pd = &lcdata->conf->rxring.pd_pool[0];
	init_pd_state(pd);
        //uint32_t qlatency;
        uint32_t depth, res32;
        uint32_t current_time = rte_get_timer_cycles() * 1000000 / rte_get_timer_hz(); // in us
        pd->wrapper = m;
        pd->data = (void*)m;
        depth = rte_ring_count(lcdata->conf->rxring.ring);
        qlatency = qdepth_bytes * 8 / 5000;

	reset_headers(pd, NULL);
	set_fld(pd, FLD(all_metadatas,qdepth_bytes), qdepth_bytes);	
	set_fld(pd, FLD(all_metadatas,qdepth), qdepth);	
	set_fld(pd, FLD(all_metadatas,avg_qdepth), avg_qdepth);	
	set_fld(pd, FLD(all_metadatas,avg_qdepth_bytes), avg_qdepth_bytes);	
	set_fld(pd, FLD(all_metadatas,qlatency), qlatency);	
	set_fld(pd, FLD(all_metadatas,timestamp), current_time);	
        
	handle_packet(0, 1, STDPARAMS_IN);// tmp_pd, lcdata->conf->state.tables, &(lcdata->conf->state.parser_state), 0, depth, qlatency, current_time, avg_qdepth/1000);
        //res32 = GET_INT32_AUTO_PACKET(tmp_pd, header_instance_standard_metadata, field_instance_all_metadatas_drop);
	//res32 = GET32(src_pkt(pd), FLD(all_metadatas,drop));
	if (GET32(dst_pkt(pd), EGRESS_META_FLD) == EGRESS_DROP_VALUE){
                        debug("ERROR :::: Dropping packet after ingress control\n");
                        return -1;
        }
        debug(" :::: PACKET OK\n");
        return 0;

}


void do_ingressing_with_aqm(LCPARAMS) {
    struct rte_mbuf* m;
    struct rte_mbuf* ma[1];
    int ret;
    int tqs;
    uint32_t current_time = rte_get_timer_cycles() * 1000000 / rte_get_timer_hz(); // in us

    unsigned queue_count = get_queue_count(lcdata);
    qdepth = rte_ring_count(lcdata->conf->rxring.ring);
    if (maxqsbyte<qdepth_bytes) {maxqsbyte = qdepth_bytes;}
    if (current_time > last_t) {
                                printf("buffer size: %d %d %d %d %u %f %d %d\n", qdepth, qdepth_bytes/1000, ingress_drop, queue_drop, avg_qdepth, 1.0*qlatency/1000.0, rx_rate, maxqsbyte/1000 ); //rte_ring_count(lcdata->conf->rxring.ring));
                                printf("buffer size: qsize:%d sqize/1000:%d ingress_drop:%d queue_drop:%d avg_qdepth:%u avg_qdepth_bytes:%u qlatency:%f rx_rate:%d maxqbyte:%d\n", qdepth, qdepth_bytes, ingress_drop, queue_drop, avg_qdepth, avg_qdepth_bytes, 1.0*qlatency/1000.0, rx_rate, maxqsbyte ); //rte_ring_count(lcdata->conf->rxring.ring));
                                fflush(stdout);
                                if (qdepth==0) avg_qdepth = 0;
                                if (qdepth==0) avg_qdepth_bytes = 0;
                                qdepth = 0;
                                last_t = current_time + 1000000;
                                if (last_t<current_time) last_t = 0;
                                ingress_drop = 0;
                                queue_drop = 0;
                                rx_rate = 0;
                                maxqsbyte = 0;
    }

    for (unsigned queue_idx = 0; queue_idx < queue_count; queue_idx++) {
        main_loop_rx_group(queue_idx, LCPARAMS_IN);
        unsigned pkt_count = get_pkt_count_in_group(lcdata);
        for (unsigned pkt_idx = 0; pkt_idx < pkt_count; pkt_idx++) {
                ++rx_rate;
                m = lcdata->pkts_burst[pkt_idx];
                rte_prefetch0(rte_pktmbuf_mtod(m, void *));
                ma[0] = m;
                struct lcore_state state = lcdata->conf->state;
                lookup_table_t** tables = state.tables;
                parser_state_t* pstate = &(state.parser_state);
                init_parser_state(&(state.parser_state));
                if (qdepth > 2000){
                        ++queue_drop;
                        rte_pktmbuf_free(m);
                }
                else if (process_pkt(m, lcdata, STDPARAMS_IN)==0) {
                        //printf("packet arrived %d\n", m->pkt_len);
                        rte_spinlock_lock(&spinlock);
                        ret = rte_ring_sp_enqueue_burst( lcdata->conf->rxring.ring,
                                                (void **) ma, //lcdata->pkts_burst[pkt_idx],
                                                1, NULL);
                        if (ret) {
                                qdepth_bytes += m->pkt_len;
                        }
                        rte_spinlock_unlock(&spinlock);
                        if (!ret) {
                                rte_pktmbuf_free(m);
                                ++queue_drop;
                                printf("QUEUE DROP: free at buffer size %d\n", rte_ring_count(lcdata->conf->rxring.ring));
                        } else {
                                avg_qdepth = (1.0-WQ)*avg_qdepth + WQ*qdepth;
                                avg_qdepth_bytes = (1.0-WQ)*avg_qdepth_bytes + WQ*qdepth_bytes;
                        }

                } else {
                        //printf("INGRESS DROP\n");
                        ++ingress_drop;
                        rte_pktmbuf_free(m);
                }
        }
    }
    rte_pause();
}

void dpdk_main_loop()
{
    extern struct lcore_conf lcore_conf[RTE_MAX_LCORE];
    uint32_t lcore_id = rte_lcore_id();

    struct lcore_data lcdata_content = init_lcore_data();
    packet_descriptor_t pd_content;

    struct lcore_data* lcdata = &lcdata_content;
    packet_descriptor_t* pd = &pd_content;

    if (!initial_check(LCPARAMS_IN))   return;

    init_dataplane(pd, lcdata->conf->state.tables);

    #if defined ASYNC_MODE && ASYNC_MODE == ASYNC_MODE_CONTEXT
        getcontext(&lcore_conf[rte_lcore_id()].main_loop_context);
    #endif

    init_stats(LCPARAMS_IN);

    int i;
    static packet_descriptor_t pd_pool[PD_POOL_SIZE];
    static uint32_t pd_idx = 0;
    //packet_descriptor_t pd;
    if ( lcore_id==0 ) { // && lcdata.conf->rxring.ring != 0) { !=2
        for (i=0;i<PD_POOL_SIZE;++i) {
            init_dataplane(&(pd_pool[i]), lcdata_content.conf->state.tables);
        }
        lcdata_content.conf->rxring.pd_idx = &pd_idx;
        lcdata_content.conf->rxring.pd_pool = pd_pool;
    }

    printf("Starting LCORE-%d\n", lcore_id);

    while (likely(core_is_working(LCPARAMS_IN))) {
        if (lcore_id == 0){
	    //main_loop_pre_do_post_rx(LCPARAMS_IN);
            main_loop_pre_rx(LCPARAMS_IN);
	    do_ingressing_with_aqm(LCPARAMS_IN);
	    //main_loop_post_rx(true, LCPARAMS_IN);
	}
	else if (lcore_id == 1){
            main_loop_pre_rx(LCPARAMS_IN);
	    do_ring_egressing(LCPARAMS_IN);
	}
	else if (lcore_id == 2){
            main_loop_pre_rx(LCPARAMS_IN);
	    do_simple_forward_for_other_port_traffic(LCPARAMS_IN);
	}
	//main_loop_post_rx(false, LCPARAMS_IN);
    }
}

static int
launch_one_lcore(__attribute__((unused)) void *dummy)
{
    dpdk_main_loop();
    return 0;
}

int launch_dpdk()
{
    #if RTE_VERSION >= RTE_VERSION_NUM(20,11,0,0)
        rte_eal_mp_remote_launch(launch_one_lcore, NULL, CALL_MAIN);

        unsigned lcore_id;
        RTE_LCORE_FOREACH_WORKER(lcore_id) {
            if (rte_eal_wait_lcore(lcore_id) < 0)
                return -1;
        }
    #else
        rte_eal_mp_remote_launch(launch_one_lcore, NULL, CALL_MASTER);

        unsigned lcore_id;
        RTE_LCORE_FOREACH_SLAVE(lcore_id) {
            if (rte_eal_wait_lcore(lcore_id) < 0)
                return -1;
        }
    #endif

    return 0;
}

void init_async()
{
    #if defined ASYNC_MODE && ASYNC_MODE != ASYNC_MODE_OFF
        RTE_LOG(INFO, P4_FWD, ":: Starter config :: \n");
        RTE_LOG(INFO, P4_FWD, " -- ASYNC_MODE: %u\n", ASYNC_MODE);
        #ifdef  DEBUG__CRYPTO_EVERY_N
            RTE_LOG(INFO, P4_FWD, " -- DEBUG__CRYPTO_EVERY_N: %u\n", DEBUG__CRYPTO_EVERY_N);
        #endif
        RTE_LOG(INFO, P4_FWD, " -- CRYPTO_NODE_MODE: %u\n", CRYPTO_NODE_MODE);
        RTE_LOG(INFO, P4_FWD, " -- FAKE_CRYPTO_SLEEP_MULTIPLIER: %u\n", FAKE_CRYPTO_SLEEP_MULTIPLIER);
        RTE_LOG(INFO, P4_FWD, " -- CRYPTO_BURST_SIZE: %u\n", CRYPTO_BURST_SIZE);
        RTE_LOG(INFO, P4_FWD, " -- CRYPTO_CONTEXT_POOL_SIZE: %u\n", CRYPTO_CONTEXT_POOL_SIZE);
        RTE_LOG(INFO, P4_FWD, " -- CRYPTO_RING_SIZE: %u\n", CRYPTO_RING_SIZE);
    #endif
}

int main(int argc, char** argv)
{
    debug("Init switch\n");

    initialize_args(argc, argv);
    initialize_nic();
    init_async();

    int launch_count2 = launch_count();
    for (int idx = 0; idx < launch_count2; ++idx) {
        debug("Init execution\n");

        init_tables();
        init_storage();

        init_memories();
        #ifndef T4P4S_NO_CONTROL_PLANE
            debug(" " T4LIT(::::,incoming) " Init control plane connection\n");
            init_control_plane();
        #else
            debug(" :::: (Control plane inactive)\n");
        #endif

        init_table_default_actions();

        t4p4s_pre_launch(idx);

        int retval = launch_dpdk();
        if (retval < 0) {
            t4p4s_abnormal_exit(retval, idx);
            return retval;
        }

        t4p4s_post_launch(idx);

        flush_tables();
    }

    return t4p4s_normal_exit();
}
