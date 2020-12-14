// SPDX-License-Identifier: Apache-2.0
// Copyright 2020 Eotvos Lorand University, Budapest, Hungary

#include "gen_include.h"
#include "main.h"

#include <rte_ethdev.h>
#include <rte_cycles.h>
#include <rte_spinlock.h>
#include <unistd.h>
#include <stdio.h>

#define BNCAP 100

#define MAXP 0x7fffffff + 1

static rte_spinlock_t spinlock = {RTE_SPINLOCK_INITIALIZER};

extern void handle_packet_begin(packet_descriptor_t* pd, lookup_table_t** tables, parser_state_t* pstate, uint32_t portid, uint32_t depth, uint32_t qlatency, uint32_t current_time, uint32_t avg_qdepth);
extern void handle_packet_end(packet_descriptor_t* pd, lookup_table_t** tables, parser_state_t* pstate);


#define PD_POOL_SIZE 1024*8
//#define LCORE_NUM 4
//packet_descriptor_t pd_pool[PD_POOL_SIZE];
//int idx_pd = 0;


volatile int qsize = 0;
uint32_t qlatency = 0;
static int avg_qdepth = 0;
int rxcnt = 0;
int txcnt = 0;

int dropcnt = 0;
int ingressdropcnt = 0;
int rxpackets = 0;
uint32_t txqsize = 0;

void init_rx_rings()
{

}

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
    for (uint8_t portidx = 0; nb_port < nb_ports - 1 && portidx < RTE_MAX_ETHPORTS; ++portidx) {
        if (portidx == ingress_port) {
           continue;
        }

        bool is_port_disabled = (port_mask & (1 << portidx)) == 0;
        if (is_port_disabled)   continue;

        packet* pkt_out = (nb_port < nb_ports) ? clone_packet(pd->wrapper, lcdata->mempool) : pd->wrapper;
        send_single_packet(pkt_out, egress_port, ingress_port, false, LCPARAMS_IN);

        nb_port++;
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
            dbg_bytes(rte_pktmbuf_mtod(mbuf, uint8_t*), rte_pktmbuf_pkt_len(mbuf), "   " T4LIT(<<,outgoing) " " T4LIT(Broadcasting,outgoing) " packet from port " T4LIT(%d,port) " to all other ports (%s) (" T4LIT(%d) " bytes): ", ingress_port, ports_msg, rte_pktmbuf_pkt_len(mbuf));
        #endif
        broadcast_packet(egress_port, ingress_port, LCPARAMS_IN);
    } else {
        dbg_bytes(rte_pktmbuf_mtod(mbuf, uint8_t*), rte_pktmbuf_pkt_len(mbuf), "   " T4LIT(<<,outgoing) " " T4LIT(Emitting,outgoing) " packet on port " T4LIT(%d,port) " (" T4LIT(%d) " bytes): ", egress_port, rte_pktmbuf_pkt_len(mbuf));
        send_single_packet(pd->wrapper, egress_port, ingress_port, false, LCPARAMS_IN);
    }
}

void do_single_tx(unsigned queue_idx, unsigned pkt_idx, LCPARAMS)
{
    if (unlikely(GET_INT32_AUTO_PACKET(pd, HDR(all_metadatas), EGRESS_META_FLD) == EGRESS_DROP_VALUE)) {
        debug(" " T4LIT(XXXX,status) " " T4LIT(Dropping,status) " packet\n");
        free_packet(LCPARAMS_IN);
    } else {
        debug(" " T4LIT(<<<<,outgoing) " " T4LIT(Egressing,outgoing) " packet\n");

        int egress_port = 1;//extract_egress_port(pd);
        int ingress_port = 0;//extract_ingress_port(pd);

        send_packet(egress_port, ingress_port, LCPARAMS_IN);
    }
}

void do_single_rx(unsigned queue_idx, unsigned pkt_idx, LCPARAMS)
{
    packet_descriptor_t* tmp_pd = &lcdata->conf->rxring.pd_pool[*(lcdata->conf->rxring.pd_idx)];
    bool got_packet = receive_packet(pkt_idx, lcdata, tmp_pd);
    int ret;
    packet_descriptor_t* tmp[1];
    tmp[0] = tmp_pd;
    rte_spinlock_lock(&spinlock);
    ret = rte_ring_sp_enqueue_burst( lcdata->conf->rxring.ring,
                                      (void **) tmp,
                                      1, NULL);
    rte_spinlock_unlock(&spinlock);
    rte_pause();
}

int process_pkt(struct rte_mbuf* m, struct lcore_data* lcdata, unsigned queue_idx)
{
   packet_descriptor_t* tmp_pd = &lcdata->conf->rxring.pd_pool[0];
   //uint32_t qlatency;
   uint32_t depth, res32;
   uint32_t current_time = rte_get_timer_cycles() * 1000000 / rte_get_timer_hz(); // in us
   tmp_pd->wrapper = m;
   tmp_pd->data = (void*)m;
   depth = rte_ring_count(lcdata->conf->rxring.ring);
   qlatency = qsize * 8 / 5000;
   handle_packet_begin(tmp_pd, lcdata->conf->state.tables, &(lcdata->conf->state.parser_state), 0, depth, qlatency, current_time, avg_qdepth/1000);
   if (unlikely(GET_INT32_AUTO_PACKET(tmp_pd, HDR(all_metadatas), EGRESS_META_FLD) == EGRESS_DROP_VALUE)) { //tmp_pd->dropped)) {
       debug(" :::: Dropping packet after ingress control\n");
       //++ingressdropcnt;
       return -1;
    }
    return 0;

}

void do_single_rx2(struct lcore_data* lcdata, packet_descriptor_t* pd, unsigned queue_idx, unsigned pkt_idx)
{
        //packet_descriptor_t* tmp_pd = &pd_pool[idx_pd];//
        packet_descriptor_t* tmp_pd = &lcdata->conf->rxring.pd_pool[*(lcdata->conf->rxring.pd_idx)];
        bool got_packet = receive_packet(pkt_idx, lcdata, tmp_pd);
        //struct rte_mbuf* mbuf = tmp_pd->wrapper;
        int ret;
        packet_descriptor_t* tmp[1];
        uint32_t depth, res32;
        uint32_t current_time = rte_get_timer_cycles() * 1000000 / rte_get_timer_hz(); // in us
        static uint32_t last_t = 0;
        static uint32_t dmax = 0;
        static uint32_t pvalmax = 0;
        uint32_t qlatency;
        int egress_port = 1; //EXTRACT_EGRESSPORT(tmp_pd);
        uint32_t value32 = (uint16_t)1;
        uint32_t tmp_v;

//      tmp_pd->wrapper = 0;
//      tmp_pd->data = 0;

        //idx_pd = (idx_pd + 1) % PD_POOL_SIZE;
//      *(lcdata->conf->rxring.pd_idx) = (*(lcdata->conf->rxring.pd_idx) + 1) % PD_POOL_SIZE;
        tmp[0] = tmp_pd;

        if (got_packet) {
                if (likely(is_packet_handled(lcdata, tmp_pd))) {
                        rxpackets++;
                        depth = rte_ring_count(lcdata->conf->rxring.ring);
                        qlatency = 1000 * depth / BNCAP; // 1500 * 8 * depth / 6000; // in us; BN = 1 MPPS
                        debug("EGRESS:: %d\n", egress_port);
                        //reset_headers(tmp_pd, 0);
                        //MODIFY_INT32_INT32_BITS_PACKET(tmp_pd, HDR(all_metadatas), FLD(all_metadatas, qdepth), depth);
                        //MODIFY_INT32_INT32_BITS_PACKET(tmp_pd, HDR(all_metadatas), FLD(all_metadatas, qlatency), qlatency);
                        //MODIFY_INT32_INT32_BITS_PACKET(tmp_pd, HDR(all_metadatas), FLD(all_metadatas, timestamp), current_time);

                        if (depth > dmax) dmax = depth;
                        //if (global_smem.prob_reg_0[0].value > pvalmax) pvalmax = global_smem.prob_reg_0[0].value;

                        if (current_time > last_t) {
//                              printf("Q-Stat %d depth: %d pkt(s)(max: %d pkts), delay: %d us, drop: %d, ingressdrop: %d, old_t: %d, qold: %d, pval: %d\n", current_time, depth, dmax,  qlatency, dropcnt, ingressdropcnt, global_smem.time_next_reg_0[0].value, global_smem.qdelay_reg_0[0].value, global_smem.prob_reg_0[0].value);
                                printf("Q-Stat time: %u depth: %d pkts(max: %d pkts), delay: %d us, undrop: %d pkts, ingress-drop: %d pps, rx-pkt: %d pps, drop-prob: %f\n", current_time, depth, dmax,  qlatency, dropcnt, ingressdropcnt, rxpackets, 1.0*ingressdropcnt/rxpackets); //, global_smem.time_next_reg_0[0].value, global_smem.qdelay_reg_0[0].value, global_smem.prob_reg_0[0].value);
                                fflush(stdout);
                                last_t = current_time + 1000000;
                                dmax = 0;
                                ingressdropcnt=0;
                                rxpackets=0;
                        }

                res32 = 0;//GET_INT32_AUTO_PACKET(tmp_pd, header_instance_standard_metadata, field_standard_metadata_t_drop);
//              tmp_pd->wrapper = mbuf;
//              tmp_pd->data = (uint8_t*)mbuf;

                if (unlikely(res32)) { //tmp_pd->dropped)) {
                        debug(" :::: Dropping packet after ingress control\n");
                        ++ingressdropcnt;
                        free_packet(LCPARAMS_IN);
                        return;
                }
                debug( "DEPTH:: %d %d\n", depth, res32);
                if ( depth < 4000) { //rte_ring_count(lcdata->conf->rxring.ring) < 500 ) {
                        *(lcdata->conf->rxring.pd_idx) = (*(lcdata->conf->rxring.pd_idx) + 1) % PD_POOL_SIZE;
                        rte_spinlock_lock(&spinlock);
                        ret = rte_ring_sp_enqueue_burst( lcdata->conf->rxring.ring,
                                                (void **) tmp,
                                                1, NULL);
                        rte_spinlock_unlock(&spinlock);
                        if (ret == 0) printf("ZERO RV\n");
        //              rte_atomic32_add(&lcdata->conf->rxring.pkt_count, ret);
                        //++(lcdata->conf->rxring.pkt_count);
//                      rxcnt +=  tmp_pd->payload_length + tmp_pd->parsed_length;
                        //debug(" :::: Queue size = " T4LIT(%d) "\n", rxcnt-txcnt);
                } else {
                        debug(" :::: Queue is full - dropping packet\n");
                        ++dropcnt;
                        //printf("------- pd_idx:%d pktind:%d p:%p\n", *(lcdata->conf->rxring.pd_idx), pkt_idx, tmp_pd->wrapper);
                        free_packet(lcdata, tmp_pd);
                }
        }
        else printf("Something went wrong -inner \n");
    }
    else printf("Something went wrong - outer \n");
}

void do_single_ring_tx(struct lcore_data* lcdata, packet_descriptor_t* pd)
{
    packet_descriptor_t* tmp[32];
    struct rte_mbuf* mtmp[1];
    int ret, nb_rx, i;
    int pkt_len;
    uint32_t value32;
    int egress_port;
    static int tbsize = 1000;
    static uint32_t last = 0; //rte_get_timer_cycles() * 1000000 / rte_get_timer_hz(); // in us;
    /*uint32_t now = rte_get_timer_cycles() * 1000000 / rte_get_timer_hz(); // in us;
    //int delta = BNCAP * (now-last) / 1000;

    //if (rte_ring_count(lcdata->conf->txring->ring)==0) return;

    //if (delta > 0) {
        tbsize += delta; //BNCAP * (now-last) / 1000;
        last = now;
    }
    if (tbsize > 1000) tbsize = 1000;


    if (tbsize <= 0) return;
   */

    rte_spinlock_lock(&spinlock);
    nb_rx = rte_ring_sc_dequeue_burst( lcdata->conf->txring->ring,
                                (void **)mtmp, 1, NULL);
    if (nb_rx==1) qsize -= mtmp[0]->pkt_len;
    rte_spinlock_unlock(&spinlock);
    //tbsize -= nb_rx;
    if (nb_rx==0) {
        rte_pause();
        return;
    }
    pkt_len = mtmp[0]->pkt_len;
    send_single_packet(mtmp[0], 1, 0, false, LCPARAMS_IN);
    //int psum = 0;
//    rte_atomic32_sub(&lcdata->conf->txring->pkt_count, nb_rx);
//    lcdata->conf->txring->byte_count += nb_rx;
    //printf("nb_rx-ring:: %d\n", nb_rx);
 /*   for (i=0;i<nb_rx;++i) {
            //egress_port = EXTRACT_EGRESSPORT(tmp[i]);
            //debug("1one EGRESS:: %d\n", egress_port);
            //handle_packet_end(tmp[i], lcdata->conf->state.tables, &(lcdata->conf->state.parser_state));
            //egress_port = EXTRACT_EGRESSPORT(tmp[i]);
            //debug("2one EGRESS:: %d\n", egress_port);
            do_single_tx(lcdata, tmp[i], 0, 0);
            //psum += tmp[i]->payload_length + tmp[i]->parsed_length;
    }*/
    //rte_pause();
    wait_for_cycles((pkt_len << 8) / 45);
    //txcnt +=  psum; //tmp_pd->payload_length + tmp_pd->parsed_length;
}


void do_rx(LCPARAMS)
{
    unsigned queue_count = get_queue_count(lcdata);
    for (unsigned queue_idx = 0; queue_idx < queue_count; queue_idx++) {
        main_loop_rx_group(queue_idx, LCPARAMS_IN);

        unsigned pkt_count = get_pkt_count_in_group(lcdata);
        for (unsigned pkt_idx = 0; pkt_idx < pkt_count; pkt_idx++) {
            do_single_rx(queue_idx, pkt_idx, LCPARAMS_IN);
        }
    }
}

void do_forward(struct lcore_data* lcdata) {
    unsigned queue_count = get_queue_count(lcdata);
    for (unsigned queue_idx = 0; queue_idx < queue_count; queue_idx++) {
        main_loop_rx_group(queue_idx, lcdata, NULL);
        unsigned pkt_count = get_pkt_count_in_group(lcdata);
        for (unsigned pkt_idx = 0; pkt_idx < pkt_count; pkt_idx++) {
	    send_single_packet(lcdata->pkts_burst[pkt_idx], 0, 1, false, lcdata, NULL);
       }
    }
}

#define WQ 0.002

void do_forward2(LCPARAMS) {
    struct rte_mbuf* m;
    struct rte_mbuf* ma[1];
    int ret;
    static int qs = 0;
    static int maxqsbyte = 0;
    int tqs;
    uint32_t current_time = rte_get_timer_cycles() * 1000000 / rte_get_timer_hz(); // in us
    static uint32_t last_t = 0;
    static int ingress_drop = 0;
    static int queue_drop = 0;
    static int rx_rate = 0;

    unsigned queue_count = get_queue_count(lcdata);
    tqs = rte_ring_count(lcdata->conf->rxring.ring);
    if (tqs>qs) { qs = tqs; }
    if (maxqsbyte<qsize) {maxqsbyte = qsize;}
    if (current_time > last_t) {
                                printf("buffer size: %d %d %d %d %u %f %d %d\n", qs, qsize/1000, ingress_drop, queue_drop, avg_qdepth/1000, 1.0*qlatency/1000.0, rx_rate, maxqsbyte/1000 ); //rte_ring_count(lcdata->conf->rxring.ring));
                                fflush(stdout);
                                if (qs==0) avg_qdepth = 0;
                                qs = 0;
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
                if (process_pkt(m, lcdata, queue_idx)==0) {
                        //printf("packet arrived %d\n", m->pkt_len);
                        rte_spinlock_lock(&spinlock);
                        ret = rte_ring_sp_enqueue_burst( lcdata->conf->rxring.ring,
                                                (void **) ma, //lcdata->pkts_burst[pkt_idx],
                                                1, NULL);
                        if (ret) {
                                qsize += m->pkt_len;
                                //printf("qsize %d\n", qsize);
                        }
                        rte_spinlock_unlock(&spinlock);
                        if (!ret) {
                                rte_pktmbuf_free(m);
                                ++queue_drop;
                                //printf("free at buffer size %d\n", rte_ring_count(lcdata->conf->rxring.ring));
                        } else {
                                avg_qdepth = (1.0-WQ)*avg_qdepth + WQ*qsize;
                        }

                } else {
                        ++ingress_drop;
                        rte_pktmbuf_free(m);
                }
        }
    }
    rte_pause();
}


void dpdk_main_loop()
{
    int i;
    uint32_t tmp;
    uint32_t lcore_id = rte_lcore_id();
    struct lcore_data lcdata_content = init_lcore_data();
    packet_descriptor_t pd_content;

    struct lcore_data* lcdata = &lcdata_content;
    packet_descriptor_t* pd = &pd_content;

    if (!lcdata->is_valid) {
        //return;
    }

    static packet_descriptor_t pd_pool[PD_POOL_SIZE];
    static uint32_t pd_idx = 0;
    //packet_descriptor_t pd;
    if ( lcore_id==0 ) { // && lcdata.conf->rxring.ring != 0) { !=2
        for (i=0;i<PD_POOL_SIZE;++i) {
            init_dataplane(&(pd_pool[i]), lcdata->conf->state.tables);
        }
        lcdata->conf->rxring.pd_idx = &pd_idx;
        lcdata->conf->rxring.pd_pool = pd_pool;
    }
    printf("Starting LCORE-%d\n", lcore_id);

    while (core_is_working(LCPARAMS_IN)) {
	if (lcore_id == 0) {
//              main_loop_pre_rx(&lcdata);
                do_forward2(LCPARAMS_IN);
//              do_rx(&lcdata, NULL);
        }
        else if (lcore_id == 1) {
                main_loop_pre_rx(LCPARAMS_IN);
                tmp = lcdata->conf->hw.tx_mbufs[1].len;
                if (tmp>txqsize) txqsize = tmp;
                //if (tmp==0) { //printf("LEN>0 %d\n", lcdata.conf->hw.tx_mbufs[1].len);
                        do_single_ring_tx(lcdata,NULL);
                //}
        } else {
                main_loop_pre_rx(LCPARAMS_IN);
                do_forward(lcdata);
        }
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

int main(int argc, char** argv)
{
    debug("Init switch\n");

    initialize_args(argc, argv);
    initialize_nic();

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
