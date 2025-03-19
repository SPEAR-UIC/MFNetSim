/* Copyright (C) 2015 University of Chicago.
 * See COPYRIGHT notice in top-level directory.
 *
 */
#include <stdio.h>
#include <string.h>
#include <stddef.h>
#include <assert.h>
#include <inttypes.h>
#include <sys/stat.h>
#include <sys/resource.h>

#include <ross.h>
#include <codes/codes.h>
#include <codes/codes_mapping.h>
#include <codes/jenkins-hash.h>
#include <codes/codes-workload.h>
#include <codes/model-net.h>
#include <codes/model-net-lp.h>
#include <codes/codes-jobmap.h>
#include <codes/resource-lp.h>
#include <codes/rc-stack.h>
#include <codes/quicklist.h>
#include <codes/quickhash.h>
#include <codes/local-storage-model.h>
#include "../codes/codes-external-store.h"
#include "../codes/codes-store-lp.h"

#define CHK_LP_NM "test-checkpoint-client"
#define MEAN_INTERVAL 550055
#define CLIENT_DBG 0
#define XIN_DBG 0
#define MAX_JOBS 10
#define dprintf(_fmt, ...) \
    do {if (CLIENT_DBG) printf(_fmt, __VA_ARGS__);} while (0)

#define CONTROL_MSG_SZ 64
#define MAX_WAIT_REQS 2048
#define RANK_HASH_TABLE_SZ 8193
#define COL_TAG 1235
#define BAR_TAG 1234
#define NEAR_ZERO .0001 

void test_checkpoint_register(void);

/* configures the lp given the global config object */
void test_checkpoint_configure(int model_net_id);

/* For communication */
tw_lpid TRACK_LP = -1;
static int unmatched = 0;
static int64_t EAGER_THRESHOLD = INT64_MAX; /* TODO: default 8192 */
static tw_stime self_overhead = 10.0;
tw_stime max_elapsed_time_per_job[MAX_JOBS] = {0};
/* On-node delay variables */
tw_stime soft_delay_mpi = 2500;
tw_stime nic_delay = 1000;
tw_stime copy_per_byte_eager = 0.55;
typedef unsigned int dumpi_req_id;
typedef struct test_checkpoint_state test_checkpoint_state;
typedef struct test_checkpoint_msg test_checkpoint_msg;

static char lp_io_dir[256] = {'\0'};
static unsigned int lp_io_use_suffix = 0;
static int do_lp_io = 0;
static int extrapolate_factor = 10;
static lp_io_handle io_handle;

static char workloads_conf_file[4096] = {'\0'};
static char alloc_file[4096] = {'\0'};
static char conf_file_name[4096] = {'\0'};
static int random_bb_nodes = 0;
static int total_checkpoints = 0;
static int PAYLOAD_SZ = 1024;
static unsigned long max_gen_data = 5000000000;

char anno[MAX_NAME_LENGTH];
char lp_grp_name[MAX_NAME_LENGTH];
char lp_name[MAX_NAME_LENGTH];
static int mapping_gid, mapping_tid, mapping_rid, mapping_offset;

int num_jobs_per_wkld[MAX_JOBS];
char wkld_type_per_job[MAX_JOBS][4096];

struct codes_jobmap_ctx *jobmap_ctx;
struct codes_jobmap_params_list jobmap_p;

/* checkpoint restart parameters */
static double checkpoint_sz;
static double checkpoint_wr_bw;
static double mtti;

static int test_checkpoint_magic;
static int cli_dfly_id;

// following is for mapping clients to servers
static int num_servers;
static int num_clients;
static int clients_per_server;
static int num_syn_clients;
static int num_chk_clients;

static double my_checkpoint_sz;
static double intm_sleep = 0;

// static int num_completed = 0;

static checkpoint_wrkld_params c_params = {0, 0, 0, 0, 0};
static char * params = NULL;

/* output logs */
static int enable_wkld_log = 0;
FILE * workload_log = NULL;
FILE * iteration_log = NULL;

/* final stats */
static tw_stime max_write_time = 0;
static tw_stime max_total_time = 0;
static tw_stime total_write_time = 0;
static tw_stime total_read_time = 0;
static tw_stime total_run_time = 0;
static double total_syn_data = 0;

unsigned long long num_bytes_sent=0;
unsigned long long num_bytes_recvd=0;
double max_time = 0,  max_comm_time = 0, max_wait_time = 0, max_send_time = 0, max_recv_time = 0;
double avg_time = 0, avg_comm_time = 0, avg_wait_time = 0, avg_send_time = 0, avg_recv_time = 0;

static tw_stime ns_to_s(tw_stime ns);
static tw_stime s_to_ns(tw_stime ns);

/* MPI_SEND_ARRIVED is issued when a MPI message arrives at its destination (the message is transported by model-net and an event is invoked when it arrives.
* MPI_SEND_POSTED is issued when a MPI message has left the source LP (message is transported via model-net). */
enum test_checkpoint_event
{
    CLI_NEXT_OP=12,
    CLI_ACK,
    CLI_WKLD_FINISH,
    CLI_BCKGND_GEN,
    CLI_BCKGND_COMPLETE,
    CLI_BCKGND_FINISH,
    /* For communication */
    MPI_SEND_ARRIVED,
    MPI_SEND_ARRIVED_CB, // for tracking message times on sender
    MPI_SEND_POSTED,
    MPI_REND_ARRIVED,
    MPI_REND_ACK_ARRIVED
};

struct test_checkpoint_state
{
    int cli_rel_id;
    int num_sent_wr;
    int num_sent_rd;
    struct codes_cb_info cb;
    int wkld_id;
    int is_finished;

    tw_stime start_time;
    tw_stime completion_time;
    int op_status_ct;
    int error_ct;
    tw_stime delayed_time;
    int num_completed_ops;
    int neighbor_completed;

    /* Number of bursts for the uniform random traffic */
    int num_bursts;

    tw_stime write_start_time;
    tw_stime total_write_time;
    tw_stime read_start_time;
    tw_stime total_read_time;

    uint64_t read_size;
    uint64_t write_size;

    int num_reads;
    int num_writes;

    /* For communication */
    long num_events_per_lp;
    struct rc_stack * processed_ops;
    struct rc_stack * processed_wait_op;
    struct rc_stack * matched_reqs;

    /* count of sends, receives, collectives and delays */
    unsigned long num_sends;
    unsigned long num_recvs;
    unsigned long num_delays;
    unsigned long num_wait;
    unsigned long num_waitall;

    double elapsed_time;
    double compute_time;    /* time spent in compute operations */
    double send_time;       /* time spent in message send/isend */
    double max_time;        /* max time for synthetic traffic message */
    double recv_time;       /* time spent in message receive */
    double wait_time;       /* time spent in wait operation */
    
    struct qlist_head arrival_queue;    /* FIFO for isend messages arrived on destination */
    struct qlist_head pending_recvs_queue;  /* FIFO for irecv messages posted but not yet matched with send operations */
    struct qlist_head completed_reqs;       /* List of completed send/receive requests */

    tw_stime cur_interval_end;
    struct pending_waits * wait_op;     /* Pending wait operation */

    /* Message size latency information */
    struct qhash_table * msg_sz_table;
    struct qlist_head msg_sz_list;

    /* quick hash for maintaining message latencies */
    unsigned long long num_bytes_sent;
    unsigned long long num_bytes_recvd;
    unsigned long long syn_data;
    unsigned long long gen_data;
  
    unsigned long prev_switch;
    int saved_perm_dest;
    unsigned long rc_perm;

    char output_buf[1024];
    char output_buf1[1024];
    int64_t syn_data_sz;
    int64_t gen_data_sz;

    /* For randomly selected BB nodes */
    int random_bb_node_id;

    /* For running multiple workloads */
    int app_id;
    int local_rank;
};

struct test_checkpoint_msg
{
    model_net_event_return event_rc;
    msg_header h;
    int payload_sz;
    int tag;
    int saved_data_sz;
    uint64_t saved_write_sz;
    uint64_t saved_read_sz;
    int saved_op_type;
    codes_store_ret_t ret;
    tw_stime saved_delay_time;
    tw_stime saved_write_time;

    /* For communication */
    int op_type;
    struct codes_workload_op * mpi_op;
    struct
    {
       tw_lpid src_rank;
       int dest_rank;
       int64_t num_bytes;
       int num_matched;
       int data_type;
       double sim_start_time;
       // for callbacks - time message was received
       double msg_send_time;
       unsigned int req_id;
       int matched_req;
       int tag;
       int app_id;
       int found_match;
       short wait_completed;
       short rend_send;
    } fwd;
    struct
    {
       int saved_perm;
       double saved_send_time;
       double saved_recv_time;
       double saved_wait_time;
       double saved_delay;
       double saved_marker_time;
       int64_t saved_num_bytes;
       int saved_syn_length;
       unsigned long saved_prev_switch;
       double saved_prev_max_time;
    } rc;
};

/* stores pointers of pending MPI operations to be matched with their respective sends/receives. */
struct mpi_msgs_queue
{
    int op_type;
    int tag;
    int source_rank;
    int dest_rank;
    int64_t num_bytes;
    int64_t seq_id;
    tw_stime req_init_time;
    dumpi_req_id req_id;
    struct qlist_head ql;
};

/* stores request IDs of completed MPI operations (Isends or Irecvs) */
struct completed_requests
{
    unsigned int req_id;
    struct qlist_head ql;
    int index;
};

/* for wait operations, store the pending operation and number of completed waits so far. */
struct pending_waits
{
    int op_type;
    unsigned int req_ids[MAX_WAIT_REQS];
    int num_completed;
    int count;
    tw_stime start_time;
    struct qlist_head ql;
};

struct msg_size_info
{
    int64_t msg_size;
    int num_msgs;
    tw_stime agg_latency;
    tw_stime avg_latency;
    struct qhash_head  hash_link;
    struct qlist_head ql; 
};

typedef struct mpi_msgs_queue mpi_msgs_queue;
typedef struct completed_requests completed_requests;
typedef struct pending_waits pending_waits;

/* set group context */
struct codes_mctx mapping_context;
enum MAPPING_CONTEXTS
{
    GROUP_RATIO=1,
    GROUP_RATIO_REVERSE,
    GROUP_DIRECT,
    GROUP_MODULO,
    GROUP_MODULO_REVERSE,
    UNKNOWN
};
static int map_ctxt = GROUP_MODULO;

static void send_ack_back(test_checkpoint_state* s, tw_bf * bf, test_checkpoint_msg * m, tw_lp * lp, mpi_msgs_queue * mpi_op, int matched_req);
static void send_ack_back_rc(test_checkpoint_state* s, tw_bf * bf, test_checkpoint_msg * m, tw_lp * lp);
/* executes MPI isend and send operations */
static void codes_exec_mpi_send(test_checkpoint_state* s, tw_bf * bf, test_checkpoint_msg * m, tw_lp* lp, struct codes_workload_op * mpi_op, int is_rend);
/* execute MPI irecv operation */
static void codes_exec_mpi_recv(test_checkpoint_state* s, tw_bf * bf, test_checkpoint_msg * m, tw_lp * lp, struct codes_workload_op * mpi_op);
/* reverse of mpi recv function. */
static void codes_exec_mpi_recv_rc(test_checkpoint_state* s, tw_bf * bf, test_checkpoint_msg* m, tw_lp* lp);
/* execute the computational delay */
static void codes_exec_comp_delay(test_checkpoint_state* s, tw_bf *bf, test_checkpoint_msg * m, tw_lp* lp, struct codes_workload_op * mpi_op);

///////////////////// HELPER FUNCTIONS FOR MPI MESSAGE QUEUE HANDLING ///////////////
/* upon arrival of local completion message, inserts operation in completed send queue */
/* upon arrival of an isend operation, updates the arrival queue of the network */
static void update_completed_queue(test_checkpoint_state* s, tw_bf * bf, test_checkpoint_msg * m, tw_lp * lp, dumpi_req_id req_id);
static void update_completed_queue_rc(test_checkpoint_state* s, tw_bf * bf, test_checkpoint_msg * m, tw_lp * lp);
static void update_arrival_queue(test_checkpoint_state* s, tw_bf* bf, test_checkpoint_msg* m, tw_lp * lp);
static void update_arrival_queue_rc(test_checkpoint_state* s, tw_bf* bf, test_checkpoint_msg* m, tw_lp * lp);
/* callback to a message sender for computing message time */
static void update_message_time(test_checkpoint_state* s, tw_bf* bf, test_checkpoint_msg* m, tw_lp * lp);
static void update_message_time_rc(test_checkpoint_state* s, tw_bf* bf, test_checkpoint_msg* m, tw_lp * lp);
static int msg_size_hash_compare(void *key, struct qhash_head *link);

void handle_next_operation_rc(test_checkpoint_state * ns, tw_lp * lp);
void handle_next_operation(test_checkpoint_state * ns, tw_lp * lp, tw_stime time);


/* convert ns to seconds */
static tw_stime ns_to_s(tw_stime ns)
{
    return(ns / (1000.0 * 1000.0 * 1000.0));
}

/* convert seconds to ns */
static tw_stime s_to_ns(tw_stime ns)
{
    return(ns * (1000.0 * 1000.0 * 1000.0));
}

static double terabytes_to_megabytes(double tib)
{
    return (tib * 1000.0 * 1000.0);
}

int get_global_id_of_job_rank(tw_lpid job_rank, int app_id)
{
    struct codes_jobmap_id lid;
    lid.job = app_id;
    lid.rank = job_rank;
    int global_rank = codes_jobmap_to_global_id(lid, jobmap_ctx);
    return global_rank;
}

/* helper function - maps an MPI rank to an LP id */
static tw_lpid rank_to_lpid(int rank)
{
    return codes_mapping_get_lpid_from_relative(rank, NULL, CHK_LP_NM, NULL, 0);
}

static void print_msgs_queue(struct qlist_head * head, int is_send)
{
    if(is_send)
        printf("\n Send msgs queue: ");
    else
        printf("\n Recv msgs queue: ");

    struct qlist_head * ent = NULL;
    mpi_msgs_queue * current = NULL;
    qlist_for_each(ent, head)
       {
            current = qlist_entry(ent, mpi_msgs_queue, ql);
            //printf(" \n Source %d Dest %d bytes %"PRId64" tag %d ", current->source_rank, current->dest_rank, current->num_bytes, current->tag);
       }
}

static void print_completed_queue(tw_lp * lp, struct qlist_head * head)
{
      struct qlist_head * ent = NULL;
      struct completed_requests* current = NULL;
      tw_output(lp, "\n");
      qlist_for_each(ent, head)
       {
            current = qlist_entry(ent, completed_requests, ql);
            tw_output(lp, " %llu ", current->req_id);
       }
}

static void kickoff_synthetic_traffic_rc(struct test_checkpoint_state * ns, tw_lp * lp)
{
    tw_rand_reverse_unif(lp->rng);
}

static void kickoff_synthetic_traffic(struct test_checkpoint_state * ns, tw_lp * lp)
{
        ns->is_finished = 0;

        double checkpoint_wr_time = (c_params.checkpoint_sz * 1024)/ c_params.checkpoint_wr_bw;
        tw_stime checkpoint_interval = sqrt(2 * checkpoint_wr_time * (c_params.mtti * 60 * 60)) - checkpoint_wr_time;
   
        if(!ns->local_rank)
            printf("\n Delay for synthetic traffic %lf ", checkpoint_interval - 2.0); 

        assert(checkpoint_interval > 0);
        tw_stime nano_secs = s_to_ns(checkpoint_interval - 2.0);
        ns->start_time = tw_now(lp) + nano_secs;
        /* Generate random traffic during the delay */
        tw_event * e;
        struct test_checkpoint_msg * m_new;
        tw_stime ts = nano_secs + tw_rand_unif(lp->rng);
        e = tw_event_new(lp->gid, ts, lp);
        m_new = tw_event_data(e);
        msg_set_header(test_checkpoint_magic, CLI_BCKGND_GEN, lp->gid, &m_new->h);    
        tw_event_send(e);
}

static void notify_background_traffic_rc(struct test_checkpoint_state * ns, tw_lp * lp, tw_bf * bf)
{
    tw_rand_reverse_unif(lp->rng); 
}
static void notify_background_traffic(struct test_checkpoint_state * ns, tw_lp * lp, tw_bf * bf, struct test_checkpoint_msg * m)
{
        bf->c0 = 1;
        
        struct codes_jobmap_id jid; 
        jid = codes_jobmap_to_local_id(ns->cli_rel_id, jobmap_ctx);
        
        /* TODO: Assuming there are two jobs */    
        int other_id = 0;
        if(jid.job == 0)
            other_id = 1;
        
        struct codes_jobmap_id other_jid;
        other_jid.job = other_id;

        int num_other_ranks = codes_jobmap_get_num_ranks(other_id, jobmap_ctx);

        tw_stime ts = (1.1 * g_tw_lookahead) + tw_rand_unif(lp->rng);
        tw_lpid global_dest_id;
 
        dprintf("\n Checkpoint LP %llu lpid %d notifying background traffic!!! ", lp->gid, ns->local_rank);
        for(int k = 0; k < num_other_ranks; k++)    
        {
            other_jid.rank = k;
            int intm_dest_id = codes_jobmap_to_global_id(other_jid, jobmap_ctx); 
            global_dest_id = codes_mapping_get_lpid_from_relative(intm_dest_id, NULL, CHK_LP_NM, NULL, 0);

            tw_event * e;
            struct test_checkpoint_msg * m_new;  
            e = tw_event_new(global_dest_id, ts, lp);
            m_new = tw_event_data(e); 
            msg_set_header(test_checkpoint_magic, CLI_BCKGND_FINISH, lp->gid, &(m_new->h));
            tw_event_send(e);   
        }
        return;
}
static void notify_neighbor_rc(struct test_checkpoint_state * ns, tw_lp * lp, tw_bf * bf)
{
       if(bf->c0)
       {
            notify_background_traffic_rc(ns, lp, bf);
       }
   
       if(bf->c1)
       {
          tw_rand_reverse_unif(lp->rng); 
       }
} 
static void notify_neighbor(struct test_checkpoint_state * ns, tw_lp * lp, tw_bf * bf, struct test_checkpoint_msg * m)
{
    bf->c0 = 0;
    bf->c1 = 0;

    int num_ranks = codes_jobmap_get_num_ranks(ns->app_id, jobmap_ctx);

    if(ns->local_rank == num_ranks - 1 
            && ns->is_finished == 1)
    {
        bf->c0 = 1;
        notify_background_traffic(ns, lp, bf, m);
        return;
    }
    
    struct codes_jobmap_id nbr_jid;
    nbr_jid.job = ns->app_id;
    tw_lpid global_dest_id;

    if(ns->is_finished == 1 && (ns->neighbor_completed == 1 || ns->local_rank == 0))
    {
        bf->c1 = 1;

        dprintf("\n Local rank %d notifying neighbor %d ", ns->local_rank, ns->local_rank+1);
        tw_stime ts = (1.1 * g_tw_lookahead) + tw_rand_unif(lp->rng);
        nbr_jid.rank = ns->local_rank + 1;
        
        /* Send a notification to the neighbor about completion */
        int intm_dest_id = codes_jobmap_to_global_id(nbr_jid, jobmap_ctx); 
        global_dest_id = codes_mapping_get_lpid_from_relative(intm_dest_id, NULL, CHK_LP_NM, NULL, 0);
       
        tw_event * e;
        struct test_checkpoint_msg * m_new;  
        e = tw_event_new(global_dest_id, ts, lp);
        m_new = tw_event_data(e); 
        msg_set_header(test_checkpoint_magic, CLI_WKLD_FINISH, lp->gid, &(m_new->h));
        tw_event_send(e);   
    }
}

static void send_req_to_store_rc(
	struct test_checkpoint_state * ns,
    tw_lp * lp,
    tw_bf * bf,
    struct test_checkpoint_msg * m)
{
        if(bf->c0)
        {
           tw_rand_reverse_unif(lp->rng);
        }
	    codes_store_send_req_rc(cli_dfly_id, lp);
        
        if(bf->c10)
        {
            ns->write_size = m->saved_write_sz;
        }
        if(bf->c5)
        {
            ns->read_size = m->saved_read_sz;
        }
}

static void send_req_to_store(
	    struct test_checkpoint_state * ns,
        tw_lp * lp,
        tw_bf * bf,
        struct codes_workload_op * op_rc,
        struct test_checkpoint_msg * m,
        int is_write)
{
    struct codes_store_request r;
    msg_header h;

    codes_store_init_req(
            is_write? CSREQ_WRITE:CSREQ_READ, 0, 0, 
	        is_write? op_rc->u.write.offset:op_rc->u.read.offset, 
            is_write? op_rc->u.write.size:op_rc->u.read.size, 
            &r);

    msg_set_header(test_checkpoint_magic, CLI_ACK, lp->gid, &h);
    int dest_server_id = ns->cli_rel_id / clients_per_server;

    if(random_bb_nodes == 1)
    {
            if(ns->random_bb_node_id == -1)
            {
                bf->c0 = 1;
                dest_server_id = tw_rand_integer(lp->rng, 0, num_servers - 1);
                ns->random_bb_node_id = dest_server_id;
            }
            else
                dest_server_id = ns->random_bb_node_id;
    }

    //printf("\n cli %d dest server id %d ", ns->cli_rel_id, dest_server_id);

    codes_store_send_req(&r, dest_server_id, lp, cli_dfly_id, CODES_MCTX_DEFAULT,
            0, &h, &ns->cb);

    //if(ns->cli_rel_id == TRACK_LP)
    //    tw_output(lp, "%llu: sent %s request\n", lp->gid, is_write ? "write" : "read");

    if(is_write)
    {
        bf->c10 = 1;
        m->saved_write_sz = ns->write_size;
        ns->write_start_time = tw_now(lp);
        ns->write_size += op_rc->u.write.size;
    }
    else
    {
        bf->c5 = 1;
        m->saved_read_sz = ns->read_size;
        ns->read_start_time = tw_now(lp);
        ns->read_size += op_rc->u.read.size;
    }
}

static int msg_size_hash_compare(void *key, struct qhash_head *link)
{
    int64_t *in_size = (int64_t *)key;
    struct msg_size_info *tmp;

    tmp = qhash_entry(link, struct msg_size_info, hash_link);
    if (tmp->msg_size == *in_size)
      return 1;

    return 0;
}

/* For communication events */
/* update the message size */
static void update_message_size(test_checkpoint_state * ns,
    tw_lp * lp,
    tw_bf * bf,
    test_checkpoint_msg * m,
    mpi_msgs_queue * qitem,
    int is_eager,
    int is_send)
{
    (void)bf;
    (void)is_eager;

    struct qhash_head * hash_link = NULL;
    tw_stime msg_init_time = qitem->req_init_time;

    if(ns->msg_sz_table == NULL)
        ns->msg_sz_table = qhash_init(msg_size_hash_compare, quickhash_64bit_hash, RANK_HASH_TABLE_SZ); 
    
    hash_link = qhash_search(ns->msg_sz_table, &(qitem->num_bytes));

    if(is_send)
        msg_init_time = m->fwd.sim_start_time;
    
    /* update hash table */
    if(!hash_link)
    {
        struct msg_size_info * msg_info = (struct msg_size_info*)malloc(sizeof(struct msg_size_info));
        msg_info->msg_size = qitem->num_bytes;
        msg_info->num_msgs = 1;
        msg_info->agg_latency = tw_now(lp) - msg_init_time;
        msg_info->avg_latency = msg_info->agg_latency;
        assert(ns->msg_sz_table);
        qhash_add(ns->msg_sz_table, &(msg_info->msg_size), &(msg_info->hash_link));
        qlist_add(&msg_info->ql, &ns->msg_sz_list);
        // printf("\n Msg size %d aggregate latency %f num messages %d ", m->fwd.num_bytes, msg_info->agg_latency, msg_info->num_msgs);
    }
    else
    {
        struct msg_size_info * tmp = qhash_entry(hash_link, struct msg_size_info, hash_link);
        tmp->num_msgs++;
        tmp->agg_latency += tw_now(lp) - msg_init_time;  
        tmp->avg_latency = (tmp->agg_latency / tmp->num_msgs);
        // printf("\n Msg size %lld aggregate latency %f num messages %d ", qitem->num_bytes, tmp->agg_latency, tmp->num_msgs);
    }
}

static void update_message_time(test_checkpoint_state * s,
        tw_bf * bf,
        test_checkpoint_msg * m,
        tw_lp * lp)
{
    (void)bf;
    (void)lp;
    m->rc.saved_send_time = s->send_time;
    s->send_time += m->fwd.msg_send_time;
}

static void update_message_time_rc(test_checkpoint_state * s,
        tw_bf * bf,
        test_checkpoint_msg * m,
        tw_lp * lp)
{
    (void)bf;
    (void)lp;
    s->send_time = m->rc.saved_send_time;
}

static int notify_posted_wait(test_checkpoint_state* s,
        tw_bf * bf, test_checkpoint_msg * m, tw_lp * lp,
        unsigned int completed_req)
{
    (void)bf;

    struct pending_waits* wait_elem = s->wait_op;
    int wait_completed = 0;

    m->fwd.wait_completed = 0;

    if(!wait_elem)
        return 0;

    int op_type = wait_elem->op_type;

    if(op_type == CODES_WK_WAIT &&
            (wait_elem->req_ids[0] == completed_req))
    {
            m->fwd.wait_completed = 1;
            wait_completed = 1;
    }
    else if(op_type == CODES_WK_WAITALL
            || op_type == CODES_WK_WAITANY
            || op_type == CODES_WK_WAITSOME)
    {
        int i;
        for(i = 0; i < wait_elem->count; i++)
        {
            if(wait_elem->req_ids[i] == completed_req)
            {
                wait_elem->num_completed++;
                if(wait_elem->num_completed > wait_elem->count)
                    printf("\n Num completed %d count %d LP %llu ",
                            wait_elem->num_completed,
                            wait_elem->count,
                            LLU(lp->gid));
                // if(wait_elem->num_completed > wait_elem->count)
                //     tw_lp_suspend(lp, 1, 0);

                if(wait_elem->num_completed >= wait_elem->count)
                {
                    if(enable_wkld_log)
                    {
                        fprintf(workload_log, "\n (%lf) APP ID %d MPI WAITALL SOURCE %d COMPLETED", 
                          tw_now(lp), s->app_id, s->local_rank);
                    }
                    wait_completed = 1;
                }
                m->fwd.wait_completed = 1; //This is just the individual request handle - not the entire wait.
            }
        }
    }
    return wait_completed;
}

static void add_completed_reqs(test_checkpoint_state * s,
        tw_lp * lp,
        int count)
{
    (void)lp;
    for(int i = 0; i < count; i++)
    {
       struct completed_requests * req = (struct completed_requests*)rc_stack_pop(s->matched_reqs);
       // turn on only if wait-all unmatched error arises in optimistic mode.
       qlist_add(&req->ql, &s->completed_reqs);
    }//end for
}

static int clear_completed_reqs(test_checkpoint_state * s,
        tw_lp * lp,
        unsigned int * reqs, int count)
{
    (void)s;
    (void)lp;

    int i, matched = 0;
    for( i = 0; i < count; i++)
    {
        struct qlist_head * ent = NULL;
        struct completed_requests * current = NULL;
        struct completed_requests * prev = NULL;

        int index = 0;
        qlist_for_each(ent, &s->completed_reqs)
        {
            if(prev)
            {
                rc_stack_push(lp, prev, free, s->matched_reqs);
                prev = NULL;
            }
            
            current = qlist_entry(ent, completed_requests, ql);
            current->index = index; 
            if(current->req_id == reqs[i])
            {
                ++matched;
                qlist_del(&current->ql);
                prev = current;
            }
            ++index;
        }

        if(prev)
        {
            rc_stack_push(lp, prev, free, s->matched_reqs);
            prev = NULL;
        }
    }
    return matched;
}

static void update_completed_queue_rc(test_checkpoint_state * s, tw_bf * bf, test_checkpoint_msg * m, tw_lp * lp)
{
    if(bf->c30)
    {
        struct qlist_head * ent = qlist_pop(&s->completed_reqs);
        completed_requests * req = qlist_entry(ent, completed_requests, ql);
        free(req);
    }
    else if(bf->c31)
    {
       struct pending_waits* wait_elem = (struct pending_waits*)rc_stack_pop(s->processed_wait_op);
       s->wait_op = wait_elem;
       s->wait_time = m->rc.saved_wait_time;
       add_completed_reqs(s, lp, m->fwd.num_matched);
       handle_next_operation_rc(s, lp);
    }
    if(m->fwd.wait_completed > 0)
           s->wait_op->num_completed--;
}

static void update_completed_queue(test_checkpoint_state * s,
        tw_bf * bf,
        test_checkpoint_msg * m,
        tw_lp * lp,
        dumpi_req_id req_id)
{
    bf->c30 = 0;
    bf->c31 = 0;
    m->fwd.num_matched = 0;

    // done waiting means that this was either the only wait in the op or the last wait in the collective op.
    int done_waiting = 0;
    done_waiting = notify_posted_wait(s, bf, m, lp, req_id);

    if(!done_waiting)
    {
        bf->c30 = 1;
        completed_requests * req = (completed_requests*)malloc(sizeof(completed_requests));
        req->req_id = req_id;
        qlist_add(&req->ql, &s->completed_reqs);

        /*if(s->cli_rel_id == (tw_lpid)TRACK_LP)
        {
            tw_output(lp, "\n Forward mode adding %d ", req_id);
            print_completed_queue(lp, &s->completed_reqs);
        }*/
    }
    else
    {
        bf->c31 = 1;
        m->fwd.num_matched = clear_completed_reqs(s, lp, s->wait_op->req_ids, s->wait_op->count);

        m->rc.saved_wait_time = s->wait_time;
        s->wait_time += (tw_now(lp) - s->wait_op->start_time);

        struct pending_waits* wait_elem = s->wait_op;
        rc_stack_push(lp, wait_elem, free, s->processed_wait_op);
        s->wait_op = NULL;

        handle_next_operation(s, lp, codes_local_latency(lp));
    }
}

/* search for a matching mpi operation and remove it from the list.
 * Record the index in the list from where the element got deleted.
 * Index is used for inserting the element once again in the queue for reverse computation. */
static int rm_matching_rcv(test_checkpoint_state * ns,
        tw_bf * bf,
        test_checkpoint_msg * m,
        tw_lp * lp,
        mpi_msgs_queue * qitem)
{
    int matched = 0;
    int index = 0;
    int is_rend = 0;
    struct qlist_head *ent = NULL;
    mpi_msgs_queue * qi = NULL;

    qlist_for_each(ent, &ns->pending_recvs_queue){
        qi = qlist_entry(ent, mpi_msgs_queue, ql);
        if(//(qi->num_bytes == qitem->num_bytes)
                //&& 
               ((qi->tag == qitem->tag) || qi->tag == -1)
                && ((qi->source_rank == qitem->source_rank) || qi->source_rank == -1))
        {
            matched = 1;
            qi->num_bytes = qitem->num_bytes;
            break;
        }
        ++index;
    }

    if(matched)
    {
        // if(enable_msg_tracking && qitem->num_bytes < EAGER_THRESHOLD)
        // {
        //     update_message_size(ns, lp, bf, m, qitem, 1, 1);
        // }
        if(qitem->num_bytes >= EAGER_THRESHOLD)
        {
            /* Matching receive found, need to notify the sender to transmit
             * the data * (only works in sequential mode)*/
            bf->c10 = 1;
            is_rend = 1;
            send_ack_back(ns, bf, m, lp, qitem, qi->req_id);
        }
        else
        {
            bf->c12 = 1;
            m->rc.saved_recv_time = ns->recv_time;
            ns->recv_time += (tw_now(lp) - m->fwd.sim_start_time);
        }
        if(qi->op_type == CODES_WK_IRECV && !is_rend)
        {
            bf->c9 = 1;
            /*if(ns->cli_rel_id == (tw_lpid)TRACK_LP)
            {
                printf("\n Completed irecv req id %d ", qi->req_id);
            }*/
            update_completed_queue(ns, bf, m, lp, qi->req_id);
        }
        else if(qi->op_type == CODES_WK_RECV && !is_rend)
        {
            bf->c8 = 1;
            handle_next_operation(ns, lp, codes_local_latency(lp));
        }

        qlist_del(&qi->ql);

        rc_stack_push(lp, qi, free, ns->processed_ops);
        return index;
    }
    return -1;
}

static int rm_matching_send(test_checkpoint_state * ns,
        tw_bf * bf,
        test_checkpoint_msg * m,
        tw_lp * lp, mpi_msgs_queue * qitem)
{
    int matched = 0;
    struct qlist_head *ent = NULL;
    mpi_msgs_queue * qi = NULL;

    int index = 0;
    qlist_for_each(ent, &ns->arrival_queue){
        qi = qlist_entry(ent, mpi_msgs_queue, ql);
        if(//(qi->num_bytes == qitem->num_bytes) // it is not a requirement in MPI that the send and receive sizes match
                // && 
        (qi->tag == qitem->tag || qitem->tag == -1)
                && ((qi->source_rank == qitem->source_rank) || qitem->source_rank == -1))
        {
            qitem->num_bytes = qi->num_bytes;
            matched = 1;
            break;
        }
        ++index;
    }

    if(matched)
    {
        // if(enable_msg_tracking && (qi->num_bytes < EAGER_THRESHOLD))
        //     update_message_size(ns, lp, bf, m, qi, 1, 0);
        
        m->fwd.matched_req = qitem->req_id;
        int is_rend = 0;
        if(qitem->num_bytes >= EAGER_THRESHOLD)
        {
            /* Matching receive found, need to notify the sender to transmit
             * the data */
            bf->c10 = 1;
            is_rend = 1;
            send_ack_back(ns, bf, m, lp, qi, qitem->req_id);
        }

        m->rc.saved_recv_time = ns->recv_time;
        ns->recv_time += (tw_now(lp) - qitem->req_init_time);

        /*if(ns->cli_rel_id == (tw_lpid)TRACK_LP && qitem->op_type == CODES_WK_IRECV)
        {
            tw_output(lp, "\n Completed recv req id %d ", qitem->req_id);
            print_completed_queue(lp, &ns->completed_reqs);
        }*/
        
        if(qitem->op_type == CODES_WK_IRECV && !is_rend)
        {
            bf->c29 = 1;
            update_completed_queue(ns, bf, m, lp, qitem->req_id);
        }
        else if(qitem->op_type == CODES_WK_RECV && !is_rend)
        {
            bf->c6 = 1;
            handle_next_operation(ns, lp, codes_local_latency(lp));
        }

        qlist_del(&qi->ql);

        rc_stack_push(lp, qi, free, ns->processed_ops);
        return index;
    }
    return -1;
}

/* reverse handler for updating arrival queue function */
static void update_arrival_queue_rc(test_checkpoint_state* s,
        tw_bf * bf,
        test_checkpoint_msg * m, tw_lp * lp)
{
    s->num_bytes_recvd -= m->fwd.num_bytes;
    num_bytes_recvd -= m->fwd.num_bytes;

    // if(bf->c1)
    //     codes_local_latency_reverse(lp);

    if(bf->c10)
        send_ack_back_rc(s, bf, m, lp);

    if(m->fwd.found_match >= 0)
    {
        mpi_msgs_queue * qi = (mpi_msgs_queue*)rc_stack_pop(s->processed_ops);
        // int queue_count = qlist_count(&s->pending_recvs_queue);

        if(m->fwd.found_match == 0)
        {
            qlist_add(&qi->ql, &s->pending_recvs_queue);
        }
        else
        {
            int index = 1;
            struct qlist_head * ent = NULL;
            qlist_for_each(ent, &s->pending_recvs_queue)
            {
               if(index == m->fwd.found_match)
               {
                 qlist_add(&qi->ql, ent);
                 break;
               }
               index++;
            }
        }
        if(bf->c12)
        {
            s->recv_time = m->rc.saved_recv_time;
        }
        
        //if(bf->c10)
        //    send_ack_back_rc(s, bf, m, lp);
        if(bf->c9)
            update_completed_queue_rc(s, bf, m, lp);
        if(bf->c8)
            handle_next_operation_rc(s, lp);
    }
    else if(m->fwd.found_match < 0)
    {
        struct qlist_head * ent = qlist_pop_back(&s->arrival_queue);
        mpi_msgs_queue * qi = qlist_entry(ent, mpi_msgs_queue, ql);
        free(qi);
    }
}

/* once an isend operation arrives, the pending receives queue is checked to find out if there is a irecv that has already been posted. 
 * If no isend has been posted. */
static void update_arrival_queue(test_checkpoint_state* s, tw_bf * bf, test_checkpoint_msg * m, tw_lp * lp)
{
    if(s->app_id != m->fwd.app_id)
        printf("\n Received message for app %d my id %d my rank %llu ",
                m->fwd.app_id, s->app_id, LLU(s->cli_rel_id));
    assert(s->app_id == m->fwd.app_id);

    //if(s->local_rank != m->fwd.dest_rank)
    //    printf("\n Dest rank %d local rank %d ", m->fwd.dest_rank, s->local_rank);
    m->rc.saved_recv_time = s->recv_time;
    s->num_bytes_recvd += m->fwd.num_bytes;
    num_bytes_recvd += m->fwd.num_bytes;

    // send a callback to the sender to increment times
    // find the global id of the source
    int global_src_id = m->fwd.src_rank;
    global_src_id = get_global_id_of_job_rank(m->fwd.src_rank, s->app_id);
    
    if(m->fwd.num_bytes < EAGER_THRESHOLD)
    {
        //tw_stime ts = codes_local_latency(lp);
        tw_stime ts = 0;
        // assert(ts > 0);
        bf->c1 = 1;
        tw_event *e_callback = tw_event_new(rank_to_lpid(global_src_id), ts, lp);
        test_checkpoint_msg *m_callback = (test_checkpoint_msg*) tw_event_data(e_callback);
        msg_set_header(test_checkpoint_magic, MPI_SEND_ARRIVED_CB, lp->gid, &m_callback->h);
        m_callback->fwd.msg_send_time = tw_now(lp) - m->fwd.sim_start_time;
        tw_event_send(e_callback);
    }
    /* Now reconstruct the queue item */
    mpi_msgs_queue * arrived_op = (mpi_msgs_queue *) malloc(sizeof(mpi_msgs_queue));
    arrived_op->req_init_time = m->fwd.sim_start_time;
    arrived_op->op_type = m->op_type;
    arrived_op->source_rank = m->fwd.src_rank;
    arrived_op->tag = m->fwd.tag;
    arrived_op->req_id = m->fwd.req_id;
    arrived_op->num_bytes = m->fwd.num_bytes;
    arrived_op->dest_rank = m->fwd.dest_rank;

    // if(s->cli_rel_id == (tw_lpid)TRACK_LP)
    //    printf("\n Send op arrived source rank %d num bytes %llu", arrived_op->source_rank,
    //            arrived_op->num_bytes);

    int found_matching_recv = rm_matching_rcv(s, bf, m, lp, arrived_op);

    if(found_matching_recv < 0)
    {
        m->fwd.found_match = -1;
        qlist_add_tail(&arrived_op->ql, &s->arrival_queue);
    }
    else
    {
        m->fwd.found_match = found_matching_recv;
        free(arrived_op);
    }
}

static void send_ack_back_rc(test_checkpoint_state* s, tw_bf * bf, test_checkpoint_msg * m, tw_lp * lp)
{
    (void)s;
    (void)bf;
    /* Send an ack back to the sender */
    model_net_event_rc2(lp, &m->event_rc);
}

static void send_ack_back(test_checkpoint_state* s, 
    tw_bf * bf, test_checkpoint_msg * m, tw_lp * lp, mpi_msgs_queue * mpi_op, int matched_req)
{
    (void)bf;

    int global_dest_rank = mpi_op->source_rank;
    global_dest_rank =  get_global_id_of_job_rank(mpi_op->source_rank, s->app_id);
    
    tw_lpid dest_rank = codes_mapping_get_lpid_from_relative(global_dest_rank, NULL, CHK_LP_NM, NULL, 0);
    /* Send a message back to sender indicating availability.*/
    test_checkpoint_msg remote_m;
    remote_m.fwd.sim_start_time = mpi_op->req_init_time;
    remote_m.fwd.dest_rank = mpi_op->dest_rank;   
    remote_m.fwd.src_rank = mpi_op->source_rank;
    remote_m.op_type = mpi_op->op_type;
    msg_set_header(test_checkpoint_magic, MPI_REND_ACK_ARRIVED, lp->gid, &remote_m.h); 
    remote_m.fwd.tag = mpi_op->tag; 
    remote_m.fwd.num_bytes = mpi_op->num_bytes;
    remote_m.fwd.req_id = mpi_op->req_id;  
    remote_m.fwd.matched_req = matched_req;

    // TODO: If we want ack messages to be in the low-latency class, change this function. -Kevin Brown 2021.02.18
    char prio[12];
    strcpy(prio, "medium"); 
    
    m->event_rc = model_net_event_mctx(cli_dfly_id, &mapping_context, &mapping_context,
        prio, dest_rank, CONTROL_MSG_SZ, (self_overhead + soft_delay_mpi + nic_delay),
    sizeof(test_checkpoint_msg), (const void*)&remote_m, 0, NULL, lp);
}

/* reverse handler of MPI wait operation */
static void codes_exec_mpi_wait_rc(test_checkpoint_state* s, tw_bf * bf, tw_lp* lp, test_checkpoint_msg * m)
{
   if(bf->c1)
    {
        completed_requests * qi = (completed_requests*)rc_stack_pop(s->processed_ops);
        if(m->fwd.found_match == 0)
        {
            qlist_add(&qi->ql, &s->completed_reqs);
        }
        else
        {
           int index = 1;
           struct qlist_head * ent = NULL;
           qlist_for_each(ent, &s->completed_reqs)
           {
                if(index == m->fwd.found_match)
                {
                    qlist_add(&qi->ql, ent);
                    break;
                }
                index++;
           }
        }
        handle_next_operation_rc(s, lp);
        return;
    }
        struct pending_waits * wait_op = s->wait_op;
        free(wait_op);
        s->wait_op = NULL;
}

/* execute MPI wait operation */
static void codes_exec_mpi_wait(test_checkpoint_state* s, 
    tw_bf * bf, test_checkpoint_msg * m, tw_lp* lp, struct codes_workload_op * mpi_op)
{
    /* check in the completed receives queue if the request ID has already been completed.*/        
    if(enable_wkld_log)
    {
        fprintf(workload_log, "\n (%lf) APP ID %d MPI WAIT POSTED SOURCE %d", 
            tw_now(lp), s->app_id, s->local_rank);
    }

    // printf("\n (%lf) APP ID %d MPI WAIT POSTED SOURCE %d", tw_now(lp), s->app_id, s->local_rank);
    assert(!s->wait_op);
    unsigned int req_id = mpi_op->u.wait.req_id;

    struct completed_requests* current = NULL;

    struct qlist_head * ent = NULL;
    int index = 0;
    qlist_for_each(ent, &s->completed_reqs)
    {
        current = qlist_entry(ent, completed_requests, ql);
        if(current->req_id == req_id)
        {
            bf->c1=1;
            qlist_del(&current->ql);
            rc_stack_push(lp, current, free, s->processed_ops);
            handle_next_operation(s, lp, codes_local_latency(lp));
            m->fwd.found_match = index;
            if(s->cli_rel_id == (tw_lpid)TRACK_LP)
            {
                tw_output(lp, "\n wait matched at post %d ", req_id);
                print_completed_queue(lp, &s->completed_reqs);
            }
            return;
        }
        ++index;
    }

    /*if(s->cli_rel_id == (tw_lpid)TRACK_LP)
    {
        tw_output(lp, "\n wait posted %llu ", req_id);
        print_completed_queue(lp, &s->completed_reqs);
    }*/
    /* If not, add the wait operation in the pending 'waits' list. */
    struct pending_waits* wait_op = (struct pending_waits*)malloc(sizeof(struct pending_waits));
    wait_op->op_type = mpi_op->op_type;
    wait_op->req_ids[0] = req_id;
    wait_op->count = 1;
    wait_op->num_completed = 0;
    wait_op->start_time = tw_now(lp);
    s->wait_op = wait_op;

    return;
}

static void codes_exec_mpi_wait_all_rc(test_checkpoint_state* s,
    tw_bf * bf, test_checkpoint_msg * m, tw_lp* lp)
{
    if(s->wait_op)
    {
        struct pending_waits * wait_op = s->wait_op;
        free(wait_op);
        s->wait_op = NULL;
    }
    else
    {
        add_completed_reqs(s, lp, m->fwd.num_matched);
        handle_next_operation_rc(s, lp);
    }
    return;
}

static void codes_exec_mpi_wait_all(test_checkpoint_state* s,
        tw_bf * bf, test_checkpoint_msg * m, tw_lp* lp, struct codes_workload_op * mpi_op)
{
    if(enable_wkld_log)
    {
        fprintf(workload_log, "\n (%lf) APP ID %d MPI WAITALL POSTED SOURCE %d", 
            tw_now(lp), s->app_id, s->local_rank);
    }

    int count = mpi_op->u.waits.count;
    /* If the count is not less than max wait reqs then stop */
    assert(count < MAX_WAIT_REQS);

    int i = 0, num_matched = 0;
    m->fwd.num_matched = 0;

    /*if(lp->gid == TRACK_LP)
    {
      printf("\n MPI Wait all posted ");
      print_waiting_reqs(mpi_op->u.waits.req_ids, count);
      print_completed_queue(lp, &s->completed_reqs);
    }*/
    /* check number of completed irecvs in the completion queue */
    for(i = 0; i < count; i++)
    {
        unsigned int req_id = mpi_op->u.waits.req_ids[i];
        struct qlist_head * ent = NULL;
        struct completed_requests* current = NULL;
        qlist_for_each(ent, &s->completed_reqs)
        {
            current = qlist_entry(ent, struct completed_requests, ql);
            if(current->req_id == req_id)
                num_matched++;
        }
    }

    m->fwd.found_match = num_matched;
    if(num_matched == count)
    {
        /* No need to post a MPI Wait all then, issue next event */
        /* Remove all completed requests from the list */
        m->fwd.num_matched = clear_completed_reqs(s, lp, mpi_op->u.waits.req_ids, count);
        struct pending_waits* wait_op = s->wait_op;
        free(wait_op);
        s->wait_op = NULL;
        handle_next_operation(s, lp, codes_local_latency(lp));
    }
    else
    {
        /* If not, add the wait operation in the pending 'waits' list. */
        struct pending_waits* wait_op = (struct pending_waits*)malloc(sizeof(struct pending_waits));
        wait_op->count = count;
        wait_op->op_type = mpi_op->op_type;
        assert(count < MAX_WAIT_REQS);

        for(i = 0; i < count; i++)
        wait_op->req_ids[i] =  mpi_op->u.waits.req_ids[i];

        wait_op->num_completed = num_matched;
        wait_op->start_time = tw_now(lp);
        s->wait_op = wait_op;
    }
    return;
}

/* Simulate delays between MPI operations */
static void codes_exec_comp_delay(test_checkpoint_state* s, 
    tw_bf *bf, test_checkpoint_msg * m, tw_lp* lp, struct codes_workload_op * mpi_op)
{
    bf->c28 = 0;
    tw_event* e;
    tw_stime ts;
    test_checkpoint_msg* msg;

    m->rc.saved_delay = s->compute_time;
    s->compute_time += mpi_op->u.delay.nsecs;
    ts = mpi_op->u.delay.nsecs;
    if (ts < 0)
        ts = 0;
    // if(ts <= g_tw_lookahead)
    // {
    //     bf->c28 = 1;
    //     // ts = g_tw_lookahead + 0.1 + tw_rand_exponential(lp->rng, noise);
    //     ts = g_tw_lookahead;
    // }

    //ts += g_tw_lookahead + 0.1 + tw_rand_exponential(lp->rng, noise);
    // assert(ts > 0);

    if(enable_wkld_log)
    {
        fprintf(workload_log, "\n (%lf) APP %d MPI DELAY SOURCE %d DURATION %lf",
            tw_now(lp), s->app_id, s->local_rank, ts);
    }
    handle_next_operation(s, lp, ts);
}

/* reverse computation operation for MPI irecv */
static void codes_exec_mpi_recv_rc(test_checkpoint_state* ns,
    tw_bf * bf,
    test_checkpoint_msg* m,
    tw_lp* lp)
{
    ns->recv_time = m->rc.saved_recv_time;

    if(bf->c11)
        handle_next_operation_rc(ns, lp);

    if(bf->c6)
        handle_next_operation_rc(ns, lp);

    if(m->fwd.found_match >= 0)
    {
        ns->recv_time = m->rc.saved_recv_time;
        //int queue_count = qlist_count(&ns->arrival_queue);

        mpi_msgs_queue * qi = (mpi_msgs_queue*)rc_stack_pop(ns->processed_ops);

        if(bf->c10)
            send_ack_back_rc(ns, bf, m, lp);
        if(m->fwd.found_match == 0)
        {
            qlist_add(&qi->ql, &ns->arrival_queue);
        }
        else 
        {
            int index = 1;
            struct qlist_head * ent = NULL;
            qlist_for_each(ent, &ns->arrival_queue)
            {
               if(index == m->fwd.found_match)
               {
                 qlist_add(&qi->ql, ent);
                 break;
               }
               index++;
            }
        }
        if(bf->c29)
        {
            update_completed_queue_rc(ns, bf, m, lp);
        }
    }
    else if(m->fwd.found_match < 0)
    {
        struct qlist_head * ent = qlist_pop_back(&ns->pending_recvs_queue);
        mpi_msgs_queue * qi = qlist_entry(ent, mpi_msgs_queue, ql);
        free(qi);
    }
}

/* Execute MPI Irecv operation (non-blocking receive) */
static void codes_exec_mpi_recv(test_checkpoint_state* s,
        tw_bf * bf,
        test_checkpoint_msg * m,
        tw_lp* lp,
        struct codes_workload_op * mpi_op)
{
    /* Once an irecv is posted, list of completed sends is checked to find a matching isend.
    If no matching isend is found, the receive operation is queued in the pending queue of
    receive operations. */

    m->rc.saved_recv_time = s->recv_time;
    m->rc.saved_num_bytes = mpi_op->u.recv.num_bytes;

    mpi_msgs_queue * recv_op = (mpi_msgs_queue*) malloc(sizeof(mpi_msgs_queue));
    recv_op->req_init_time = tw_now(lp);
    recv_op->op_type = mpi_op->op_type;
    recv_op->source_rank = mpi_op->u.recv.source_rank;
    recv_op->dest_rank = mpi_op->u.recv.dest_rank;
    recv_op->num_bytes = mpi_op->u.recv.num_bytes;
    recv_op->tag = mpi_op->u.recv.tag;
    recv_op->req_id = mpi_op->u.recv.req_id;

    if(enable_wkld_log)
    {
        if(mpi_op->op_type == CODES_WK_RECV)
        {
            fprintf(workload_log, "\n (%lf) APP %d MPI RECV SOURCE %d DEST %d BYTES %"PRId64,
                  tw_now(lp), s->app_id, recv_op->source_rank, recv_op->dest_rank, recv_op->num_bytes);
        }
        else
        {
            fprintf(workload_log, "\n (%lf) APP ID %d MPI IRECV SOURCE %d DEST %d BYTES %"PRId64,
                  tw_now(lp), s->app_id, recv_op->source_rank, recv_op->dest_rank, recv_op->num_bytes);
        }
    }

    int found_matching_sends = rm_matching_send(s, bf, m, lp, recv_op);

    /* for mpi irecvs, this is a non-blocking receive so just post it and move on with the trace read. */
    if(mpi_op->op_type == CODES_WK_IRECV)
    {
        bf->c6 = 1;
        handle_next_operation(s, lp, codes_local_latency(lp));
    }

    /* save the req id inserted in the completed queue for reverse computation. */
    if(found_matching_sends < 0)
    {
        m->fwd.found_match = -1;
        qlist_add_tail(&recv_op->ql, &s->pending_recvs_queue);
    }
    else
    {
        //bf->c6 = 1;
        m->fwd.found_match = found_matching_sends;
    }
}

static void codes_exec_mpi_send_rc(test_checkpoint_state * s, 
    tw_bf * bf, test_checkpoint_msg * m, tw_lp * lp)
{
    if(bf->c15 || bf->c16)
    {
        s->num_sends--;
    }

    if (bf->c15)
        model_net_event_rc2(lp, &m->event_rc);
    if (bf->c16)
        model_net_event_rc2(lp, &m->event_rc);
    if (bf->c17)
        model_net_event_rc2(lp, &m->event_rc);

    if(bf->c4)
        handle_next_operation_rc(s, lp);

    if(bf->c3)
    {
        s->num_bytes_sent -= m->rc.saved_num_bytes;
        num_bytes_sent -= m->rc.saved_num_bytes;
    }
}

/* executes MPI send and isend operations */
static void codes_exec_mpi_send(test_checkpoint_state* s,
    tw_bf * bf,
    test_checkpoint_msg * m,
    tw_lp* lp,
    struct codes_workload_op * mpi_op,
    int is_rend)
{
    bf->c3 = 0;
    bf->c1 = 0;
    bf->c4 = 0;
   
    /* Class names in the CODES dragonfly-dally (as at 2020/09/21 - KB):
     *  "high"      <- highest priority
     *  "medium"
     *  "low"
     *  "class3"
     *  "class4"
     *  "class5"
     *
     * The name of the first three classes are kept for backwards compatibility. TODO: Rename classes
     */
    char prio[12];
    strcpy(prio, "medium");

    int is_eager = 0;
    /* model-net event */
    int global_dest_rank = mpi_op->u.send.dest_rank;
    global_dest_rank = get_global_id_of_job_rank(mpi_op->u.send.dest_rank, s->app_id);

    if(s->cli_rel_id == TRACK_LP)
        printf("\n Sender rank %llu global dest rank %d dest-rank %d bytes %"PRIu64" Tag %d", LLU(s->cli_rel_id), global_dest_rank, mpi_op->u.send.dest_rank, mpi_op->u.send.num_bytes, mpi_op->u.send.tag);
    m->rc.saved_num_bytes = mpi_op->u.send.num_bytes;
    /* model-net event */
    tw_lpid dest_rank = codes_mapping_get_lpid_from_relative(global_dest_rank, NULL, CHK_LP_NM, NULL, 0);

    test_checkpoint_msg local_m;
    test_checkpoint_msg remote_m;

    local_m.fwd.dest_rank = mpi_op->u.send.dest_rank;
    local_m.fwd.src_rank = mpi_op->u.send.source_rank;
    local_m.op_type = mpi_op->op_type;
    msg_set_header(test_checkpoint_magic, MPI_SEND_POSTED, lp->gid, &local_m.h);  
    local_m.fwd.tag = mpi_op->u.send.tag;
    local_m.fwd.rend_send = 0;
    local_m.fwd.num_bytes = mpi_op->u.send.num_bytes;
    local_m.fwd.req_id = mpi_op->u.send.req_id;
    local_m.fwd.app_id = s->app_id;
    local_m.fwd.matched_req = m->fwd.matched_req;        
   
    if(mpi_op->u.send.num_bytes < EAGER_THRESHOLD)
    {
        /* directly issue a model-net send */
        bf->c15 = 1;
        is_eager = 1;
        s->num_sends++;
        tw_stime copy_overhead = copy_per_byte_eager * mpi_op->u.send.num_bytes;
        local_m.fwd.sim_start_time = tw_now(lp);

        remote_m = local_m;
        // remote_m.msg_type = MPI_SEND_ARRIVED;
        msg_set_header(test_checkpoint_magic, MPI_SEND_ARRIVED, lp->gid, &remote_m.h);  
        remote_m.fwd.app_id = s->app_id;
        m->event_rc = model_net_event_mctx(cli_dfly_id, &mapping_context, &mapping_context, 
            prio, dest_rank, mpi_op->u.send.num_bytes, (self_overhead + copy_overhead + soft_delay_mpi + nic_delay),
        sizeof(test_checkpoint_msg), (const void*)&remote_m, sizeof(test_checkpoint_msg), (const void*)&local_m, lp);
    }
    else if (is_rend == 0)
    {
        /* Initiate the handshake. Issue a control message to the destination first. No local message,
         * only remote message sent. */
        bf->c16 = 1;
        s->num_sends++;
        remote_m.fwd.sim_start_time = tw_now(lp);
        remote_m.fwd.dest_rank = mpi_op->u.send.dest_rank;   
        remote_m.fwd.src_rank = mpi_op->u.send.source_rank;
        // remote_m.msg_type = MPI_SEND_ARRIVED;
        msg_set_header(test_checkpoint_magic, MPI_SEND_ARRIVED, lp->gid, &remote_m.h); 
        remote_m.op_type = mpi_op->op_type;
        remote_m.fwd.tag = mpi_op->u.send.tag; 
        remote_m.fwd.num_bytes = mpi_op->u.send.num_bytes;
        remote_m.fwd.req_id = mpi_op->u.send.req_id;  
        remote_m.fwd.app_id = s->app_id;

        m->event_rc = model_net_event_mctx(cli_dfly_id, &mapping_context, &mapping_context, 
            prio, dest_rank, CONTROL_MSG_SZ, (self_overhead + soft_delay_mpi + nic_delay),
        sizeof(test_checkpoint_msg), (const void*)&remote_m, 0, NULL, lp);
    }
    else if(is_rend == 1)
    {
        bf->c17 = 1;
        /* initiate the actual data transfer, local completion message is sent
         * for any blocking sends. */
        local_m.fwd.sim_start_time = mpi_op->sim_start_time;
        local_m.fwd.rend_send = 1;
        remote_m = local_m; 
        // remote_m.msg_type = MPI_REND_ARRIVED;
        msg_set_header(test_checkpoint_magic, MPI_REND_ARRIVED, lp->gid, &remote_m.h); 

        m->event_rc = model_net_event_mctx(cli_dfly_id, &mapping_context, &mapping_context, 
            prio, dest_rank, mpi_op->u.send.num_bytes, (self_overhead + soft_delay_mpi + nic_delay),
        sizeof(test_checkpoint_msg), (const void*)&remote_m, sizeof(test_checkpoint_msg), (const void*)&local_m, lp);
    }
    if(enable_wkld_log && !is_rend)
    {
        if(mpi_op->op_type == CODES_WK_ISEND)
        {
            fprintf(workload_log, "\n (%lf) APP %d MPI ISEND SOURCE %llu DEST %d TAG %d BYTES %"PRId64,
                    tw_now(lp), s->app_id, LLU(remote_m.fwd.src_rank), remote_m.fwd.dest_rank, 
                    mpi_op->u.send.tag, mpi_op->u.send.num_bytes);
        }
        else
        {
            fprintf(workload_log, "\n (%lf) APP ID %d MPI SEND SOURCE %llu DEST %d TAG %d BYTES %"PRId64,
                    tw_now(lp), s->app_id, LLU(remote_m.fwd.src_rank), remote_m.fwd.dest_rank, 
                    mpi_op->u.send.tag, mpi_op->u.send.num_bytes);
        }
    }
    if(is_rend || is_eager)    
    {
       bf->c3 = 1;
       s->num_bytes_sent += mpi_op->u.send.num_bytes;
       num_bytes_sent += mpi_op->u.send.num_bytes;
    }
    /* isend executed, now get next MPI operation from the queue */
    if(mpi_op->op_type == CODES_WK_ISEND && (!is_rend || is_eager))
    {
       bf->c4 = 1;
       handle_next_operation(s, lp, codes_local_latency(lp));
    }
}



void handle_next_operation_rc(
	struct test_checkpoint_state * ns, 
	tw_lp * lp)
{
	ns->op_status_ct++;
}

/* Add a certain delay event */
void handle_next_operation(
	struct test_checkpoint_state * ns,
	tw_lp * lp,
	tw_stime time)
{
    ns->op_status_ct--;
 
    /* Issue another next event after a certain time */
    tw_event * e;

    struct test_checkpoint_msg * m_new;
    e = tw_event_new(lp->gid, time, lp);
    m_new = tw_event_data(e);
    msg_set_header(test_checkpoint_magic, CLI_NEXT_OP, lp->gid, &m_new->h);    
    tw_event_send(e);
    return;
}

void complete_random_traffic_rc(
    struct test_checkpoint_state * ns,
    tw_bf * b,
    struct test_checkpoint_msg * msg,
    tw_lp * lp)
{
    ns->syn_data_sz -= msg->payload_sz;
}

void complete_random_traffic(
    struct test_checkpoint_state * ns,
    tw_bf * b,
    struct test_checkpoint_msg * msg,
    tw_lp * lp)
{
    assert(ns->app_id != -1);
   /* Record in some statistics? */ 
    ns->syn_data_sz += msg->payload_sz;
}
void finish_bckgnd_traffic_rc(
    struct test_checkpoint_state * ns,
    tw_bf * b,
    struct test_checkpoint_msg * msg,
    tw_lp * lp)
{
        ns->num_bursts--;
        ns->is_finished = 0;

        if(b->c0)
        {
            ns->gen_data_sz = msg->saved_data_sz;
            kickoff_synthetic_traffic_rc(ns, lp);
        }
        return;
}
void finish_bckgnd_traffic(
    struct test_checkpoint_state * ns,
    tw_bf * b,
    struct test_checkpoint_msg * msg,
    tw_lp * lp)
{
        if(ns->num_bursts > total_checkpoints || ns->num_bursts < 0)
        {
            tw_lp_suspend(lp, 0, 0);
            return;
        }
        ns->num_bursts++;
        ns->is_finished = 1;

        dprintf("\n LP %llu completed sending data %lld completed at time %lf ", lp->gid, ns->gen_data_sz, tw_now(lp));

        if(ns->num_bursts < total_checkpoints)
        {
            b->c0 = 1;
            msg->saved_data_sz = ns->gen_data_sz;
            ns->gen_data_sz = 0;
            kickoff_synthetic_traffic(ns, lp);
        }
       return;
}

void finish_nbr_wkld_rc(
    struct test_checkpoint_state * ns,
    tw_bf * b,
    struct test_checkpoint_msg * msg,
    tw_lp * lp)
{
    ns->neighbor_completed = 0;
    
    notify_neighbor_rc(ns, lp, b);
}

void finish_nbr_wkld(
    struct test_checkpoint_state * ns,
    tw_bf * b,
    struct test_checkpoint_msg * msg,
    tw_lp * lp)
{
    ns->neighbor_completed = 1;

    notify_neighbor(ns, lp, b, msg);
}

void generate_random_traffic_rc(
      struct test_checkpoint_state * ns,
      tw_bf * b,
      struct test_checkpoint_msg * msg,
      tw_lp * lp)
{
    if(b->c8)
        return;

    tw_rand_reverse_unif(lp->rng);
    tw_rand_reverse_unif(lp->rng);
    
    //model_net_event_rc2(cli_dfly_id, lp, PAYLOAD_SZ);
    model_net_event_rc2(lp, &msg->event_rc);
    ns->gen_data_sz -= PAYLOAD_SZ;
}

void generate_random_traffic(
    struct test_checkpoint_state * ns,
    tw_bf * b,
    struct test_checkpoint_msg * msg,
    tw_lp * lp)
{
    if(ns->is_finished == 1 || ns->gen_data_sz >= max_gen_data) 
    {
        b->c8 = 1;
        return;
    }
    //printf("\n LP %ld: completed %ld messages ", lp->gid, ns->num_random_pckts);
    tw_lpid global_dest_id; 
   /* Get job information */
   struct codes_jobmap_id jid; 
   jid = codes_jobmap_to_local_id(ns->cli_rel_id, jobmap_ctx); 

   int dest_svr = tw_rand_integer(lp->rng, 0, num_syn_clients - 1);
   if(dest_svr == ns->local_rank)
       dest_svr = (dest_svr + 1) % num_syn_clients;

   struct test_checkpoint_msg * m_remote = malloc(sizeof(struct test_checkpoint_msg));
   msg_set_header(test_checkpoint_magic, CLI_BCKGND_COMPLETE, lp->gid, &(m_remote->h));

   codes_mapping_get_lp_info(lp->gid, lp_grp_name, &mapping_gid, lp_name, &mapping_tid, NULL, &mapping_rid, &mapping_offset);

   jid.rank = dest_svr;
   int intm_dest_id = codes_jobmap_to_global_id(jid, jobmap_ctx);

   global_dest_id = codes_mapping_get_lpid_from_relative(intm_dest_id, NULL, CHK_LP_NM, NULL, 0);

   m_remote->payload_sz = PAYLOAD_SZ;
   ns->gen_data_sz += PAYLOAD_SZ;

   msg->event_rc  = model_net_event(cli_dfly_id, "synthetic-tr", global_dest_id, PAYLOAD_SZ, 0.0, 
           sizeof(struct test_checkpoint_msg), (const void*)m_remote, 
           0, NULL, lp);

   /* New event after MEAN_INTERVAL */
    tw_stime ts = MEAN_INTERVAL + tw_rand_unif(lp->rng); 
    tw_event * e;
    struct test_checkpoint_msg * m_new;
    e = tw_event_new(lp->gid, ts, lp);
    m_new = tw_event_data(e);
    msg_set_header(test_checkpoint_magic, CLI_BCKGND_GEN, lp->gid, &(m_new->h));    
    tw_event_send(e);
}

static void next_checkpoint_op(
        struct test_checkpoint_state * ns,
        tw_bf * bf,
        struct test_checkpoint_msg * msg,
        tw_lp * lp)
{
    assert(ns->app_id != -1);

    if(ns->op_status_ct > 0)
    {
        char buf[64];
        int written = sprintf(buf, "I/O workload operator error: %"PRIu64"  \n",lp->gid);

        lp_io_write(lp->gid, "errors %d %s ", written, buf);
        return;
    }

    ns->op_status_ct++;

    struct codes_workload_op * op_rc = malloc(sizeof(struct codes_workload_op));
    codes_workload_get_next(ns->wkld_id, ns->app_id, ns->local_rank, op_rc);
    msg->mpi_op = op_rc; 
    msg->op_type = op_rc->op_type;

    msg->saved_op_type = op_rc->op_type;
    if(op_rc->op_type == CODES_WK_END)
    {
	    ns->completion_time = tw_now(lp);
        ns->elapsed_time = tw_now(lp) - ns->start_time;
        ns->is_finished = 1; 
        
        /* Notify ranks from other job that checkpoint traffic has completed */
        int num_jobs = codes_jobmap_get_num_jobs(jobmap_ctx); 
        if(num_jobs <= 1)
        {
            bf->c9 = 1;
            return;
        }
        /* Now if notification has been received from the previous rank that
         * checkpoint has completed then we send a notification to the next
         * rank */
         notify_neighbor(ns, lp, bf, msg);
         
        if(ns->cli_rel_id == TRACK_LP)
            tw_output(lp, "\nClient rank %d completed workload.", ns->cli_rel_id);
		
        return;
    }
    switch(op_rc->op_type)
    {
    	case CODES_WK_BARRIER:
    	{
            if(ns->cli_rel_id == TRACK_LP)
    		   tw_output(lp, "\nClient rank %d hit barrier.", ns->cli_rel_id);
    		
            handle_next_operation(ns, lp, codes_local_latency(lp));
    	}
        break;

    	case CODES_WK_DELAY:
    	{
            ns->num_delays++;
            // if(ns->cli_rel_id == TRACK_LP)
    		//     tw_output(lp, "Client rank %d will delay for %lf seconds.\n", ns->cli_rel_id,
            //           op_rc->u.delay.seconds);
            codes_exec_comp_delay(ns, bf, msg, lp, op_rc);
    	}
    	break;

        case CODES_WK_OPEN:
    	{
            if(ns->cli_rel_id == TRACK_LP)
    		   tw_output(lp, "\nClient rank %d will open file id %llu", ns->cli_rel_id,
    		    op_rc->u.open.file_id);
    		handle_next_operation(ns, lp, codes_local_latency(lp));
    	}
    	break;
    	
    	case CODES_WK_CLOSE:
    	{	
            if(ns->cli_rel_id == TRACK_LP)
    		    tw_output(lp, "\nClient rank %d will close file id %llu", ns->cli_rel_id,
                        op_rc->u.close.file_id);
    		handle_next_operation(ns, lp, codes_local_latency(lp));
    	}
    	break;
    	case CODES_WK_WRITE:
    	{
            if(ns->cli_rel_id == TRACK_LP)
    		    tw_output(lp, "\nClient rank %d initiate write operation size %ld offset %ld .", ns->cli_rel_id, 
    	            op_rc->u.write.size, op_rc->u.write.offset);
    		send_req_to_store(ns, lp, bf, op_rc, msg, 1);
        }	
    	break;

    	case CODES_WK_READ:
    	{
            if(ns->cli_rel_id == TRACK_LP)
    		    tw_output(lp, "\nClient rank %d initiate read operation size %ld offset %ld .", ns->cli_rel_id, op_rc->u.read.size, op_rc->u.read.offset);
                    ns->num_sent_rd++;
    		send_req_to_store(ns, lp, bf, op_rc, msg, 0);
        }
    	break;

        case CODES_WK_SEND:
        case CODES_WK_ISEND:
        {
            if(ns->cli_rel_id == TRACK_LP)
                tw_output(lp, "\nClient rank %d initiate send operation size %"PRIu64" dst %d Tag %d.", ns->cli_rel_id, op_rc->u.send.num_bytes, op_rc->u.send.dest_rank, op_rc->u.send.tag);
            codes_exec_mpi_send(ns, bf, msg, lp, op_rc, 0);
        }
        break;

        case CODES_WK_RECV:
        case CODES_WK_IRECV:
        {
            ns->num_recvs++;
            if(ns->cli_rel_id == TRACK_LP)
                tw_output(lp, "\nClient rank %d initiate recv operation size %"PRIu64" dst %d Tag %d.", ns->cli_rel_id, op_rc->u.recv.num_bytes, op_rc->u.recv.dest_rank, op_rc->u.recv.tag);
            codes_exec_mpi_recv(ns, bf, msg, lp, op_rc);
        }
        break;

        case CODES_WK_WAIT:
        {
            ns->num_wait++;
            if(ns->cli_rel_id == TRACK_LP)
                tw_output(lp, "\nClient rank %d initiate wait operation reqid %d .", ns->cli_rel_id, op_rc->u.wait.req_id);
            codes_exec_mpi_wait(ns, bf, msg, lp, op_rc);
        }
        break;

        case CODES_WK_WAITALL:
        {
            ns->num_waitall++;
            codes_exec_mpi_wait_all(ns, bf, msg, lp, op_rc);
        }
        break;
        
        case CODES_WK_MARK:
        {
            fprintf(iteration_log, "ITERATION %d node %llu job %d rank %d time %lf\n", op_rc->u.send.tag, 
                    LLU(ns->cli_rel_id), ns->app_id, ns->local_rank, tw_now(lp));
            msg->rc.saved_marker_time = tw_now(lp);
            handle_next_operation(ns, lp, codes_local_latency(lp));
        }
        break;

        default:
            printf("\n Unknown client operation %d ", op_rc->op_type);
    }
    free(op_rc);
	
}
static void next_checkpoint_op_rc(
        struct test_checkpoint_state *ns,
        tw_bf * bf,
        struct test_checkpoint_msg *m,
        tw_lp *lp)
{
    ns->op_status_ct--;
    codes_workload_get_next_rc2(ns->wkld_id, ns->app_id, ns->local_rank);

    if(m->saved_op_type == CODES_WK_END)
    {
        ns->is_finished = 0; 
       
        if(bf->c9)
            return;

        notify_neighbor_rc(ns, lp, bf);
        return;
    }
    switch(m->saved_op_type) 
    {
        case CODES_WK_READ:
        {
            ns->num_sent_rd--;
            send_req_to_store_rc(ns, lp, bf, m);
        }
        break;

        case CODES_WK_WRITE:
        {
            ns->num_sent_wr--;
            send_req_to_store_rc(ns, lp, bf, m);
        }
        break;

        case CODES_WK_DELAY:
        {
            ns->num_delays--;
            ns->compute_time = m->rc.saved_delay;
            // handle_next_operation_rc(ns, lp); 
        }
        break;

        case CODES_WK_BARRIER:
        case CODES_WK_OPEN:
        case CODES_WK_CLOSE:
        {
            codes_local_latency_reverse(lp);
            handle_next_operation_rc(ns, lp);
        }
        break;

        case CODES_WK_SEND:
        case CODES_WK_ISEND:
        {
            codes_exec_mpi_send_rc(ns, bf, m, lp);
        }
        break;

        case CODES_WK_RECV:
        case CODES_WK_IRECV:
        {
            codes_exec_mpi_recv_rc(ns, bf, m, lp);
            ns->num_recvs--;
        }
        break;

        case CODES_WK_WAIT:
        {
            ns->num_wait--;
            codes_exec_mpi_wait_rc(ns, bf, lp, m);
        }
        break;

        case CODES_WK_WAITALL:
        {
            ns->num_waitall--;
            codes_exec_mpi_wait_all_rc(ns, bf, m, lp);
        }
        break;

        case CODES_WK_MARK:
        {
            // printf("\n MARK_%d node %llu job %d rank %d time %lf \n", mpi_op->u.send.tag, LLU(s->cli_rel_id), s->app_id, s->local_rank, tw_now(lp));
            // m->rc.saved_marker_time = tw_now(lp);
            // fprintf(iteration_log, "ITERATION %d node %llu job %d rank %d time %lf\n", mpi_op->u.send.tag, LLU(s->cli_rel_id), s->app_id, s->local_rank, tw_now(lp));
            // m->rc.saved_marker_time = tw_now(lp);
            handle_next_operation_rc(ns, lp); 
        }
        break;
        
        default:
	       printf("\n Unknown client operation reverse %d", m->saved_op_type);
    }
}


static void test_checkpoint_event(
        struct test_checkpoint_state * ns,
        tw_bf * b,
        struct test_checkpoint_msg * m,
        tw_lp * lp)
{
#if CLIENT_DEBUG
    fprintf(ns->fdbg, "event num %d\n", ns->event_num);
#endif
    if (ns->error_ct > 0){
        ns->error_ct++;
        return;
    }
    if(m->h.magic != test_checkpoint_magic)
        printf("\n Msg magic %d checkpoint magic %d event-type %d src %"PRIu64" ", m->h.magic,
               test_checkpoint_magic,
               m->h.event_type,
               m->h.src);

    assert(m->h.magic == test_checkpoint_magic);

    rc_stack_gc(lp, ns->matched_reqs);
    rc_stack_gc(lp, ns->processed_ops);
    rc_stack_gc(lp, ns->processed_wait_op);

    switch(m->h.event_type) 
    {
    	case CLI_NEXT_OP:
    		next_checkpoint_op(ns, b, m, lp);
    	break;

    	case CLI_ACK:
            ns->num_completed_ops++;
    	    m->saved_write_time = ns->total_write_time;
            ns->total_write_time += (tw_now(lp) - ns->write_start_time);
            handle_next_operation(ns, lp, codes_local_latency(lp));
        break;

        case CLI_BCKGND_GEN:
            generate_random_traffic(ns, b, m, lp);
            break;

        case CLI_BCKGND_COMPLETE:
            complete_random_traffic(ns, b, m, lp);
            break;

        case CLI_BCKGND_FINISH:
            finish_bckgnd_traffic(ns, b, m, lp);
            break;

        case CLI_WKLD_FINISH:
            finish_nbr_wkld(ns, b, m, lp);
            break;
        
        case MPI_SEND_ARRIVED:
            update_arrival_queue(ns, b, m, lp);
        break;

        case MPI_REND_ARRIVED:
        {
            /* update time of messages */
            mpi_msgs_queue mpi_op;
            mpi_op.op_type = m->op_type;
            mpi_op.tag = m->fwd.tag;
            mpi_op.num_bytes = m->fwd.num_bytes;
            mpi_op.source_rank = m->fwd.src_rank;
            mpi_op.dest_rank = m->fwd.dest_rank;
            mpi_op.req_init_time = m->fwd.sim_start_time;
           
            // if(enable_msg_tracking)
            //     update_message_size(s, lp, b, m, &mpi_op, 0, 1);
        
            int global_src_id = m->fwd.src_rank;
            global_src_id = get_global_id_of_job_rank(m->fwd.src_rank, ns->app_id);
            
            tw_event *e_callback =tw_event_new(rank_to_lpid(global_src_id), 0, lp);
            test_checkpoint_msg *m_callback = (test_checkpoint_msg*)tw_event_data(e_callback);
            // m_callback->msg_type = MPI_SEND_ARRIVED_CB;
            msg_set_header(test_checkpoint_magic, MPI_SEND_ARRIVED_CB, lp->gid, &m_callback->h);  
            m_callback->fwd.msg_send_time = tw_now(lp) - m->fwd.sim_start_time;
            tw_event_send(e_callback);
           
            /* request id pending completion */
            if(m->fwd.matched_req >= 0)
            {
                b->c8 = 1;
                update_completed_queue(ns, b, m, lp, m->fwd.matched_req);
            }
            else /* blocking receive pending completion*/
            {
                b->c10 = 1;
                handle_next_operation(ns, lp, codes_local_latency(lp));
            }
            
            m->rc.saved_recv_time = ns->recv_time;
            ns->recv_time += (tw_now(lp) - m->fwd.sim_start_time);
        }
        break;

        case MPI_REND_ACK_ARRIVED:
        {
            /* reconstruct the op and pass it on for actual data transfer */ 
            int is_rend = 1;

            struct codes_workload_op mpi_op;
            mpi_op.op_type = m->op_type;
            mpi_op.u.send.tag = m->fwd.tag;
            mpi_op.u.send.num_bytes = m->fwd.num_bytes;
            mpi_op.u.send.source_rank = m->fwd.src_rank;
            mpi_op.u.send.dest_rank = m->fwd.dest_rank;
            mpi_op.sim_start_time = m->fwd.sim_start_time;
            mpi_op.u.send.req_id = m->fwd.req_id;

            codes_exec_mpi_send(ns, b, m, lp, &mpi_op, is_rend);
        }
        break;        

        case MPI_SEND_ARRIVED_CB:
            update_message_time(ns, b, m, lp);
        break;

        case MPI_SEND_POSTED:
        {
           int is_eager = 0;
           if(m->fwd.num_bytes < EAGER_THRESHOLD)
                is_eager = 1;

           if(m->op_type == CODES_WK_SEND && (is_eager == 1 || m->fwd.rend_send == 1))
           {
                b->c29 = 1;
                handle_next_operation(ns, lp, codes_local_latency(lp));
           }
           else if(m->op_type == CODES_WK_ISEND && (is_eager == 1 || m->fwd.rend_send == 1))
            {
                // tw_output(lp, "\n isend req id %llu ", m->fwd.req_id);
                b->c28 = 1;
                update_completed_queue(ns, b, m, lp, m->fwd.req_id);
            }
        }
        break;


        default:
            assert(0);
    }
}
static void test_checkpoint_event_rc(
        struct test_checkpoint_state * ns,
        tw_bf * b,
        struct test_checkpoint_msg * m,
        tw_lp * lp)
{
    assert(m->h.magic == test_checkpoint_magic);

    if (ns->error_ct > 0){
        ns->error_ct--;
        if (ns->error_ct==0){
            lp_io_write_rev(lp->gid, "errors");
#if CLIENT_DEBUG
            fprintf(ns->fdbg, "left bad state through reverse\n");
#endif
        }
        return;
    }
    switch(m->h.event_type) {
    	case CLI_NEXT_OP:	
    		next_checkpoint_op_rc(ns, b, m, lp);
    	break;
    	
    	case CLI_ACK:
    		ns->num_completed_ops--;
            ns->total_write_time = m->saved_write_time;
            handle_next_operation_rc(ns, lp);
    	break;

        case CLI_BCKGND_GEN:
            generate_random_traffic_rc(ns, b, m, lp);
        break;

       case CLI_BCKGND_COMPLETE:
            complete_random_traffic_rc(ns, b, m, lp);
        break;

        case CLI_BCKGND_FINISH:
            finish_bckgnd_traffic_rc(ns, b, m, lp);
        break;

        case CLI_WKLD_FINISH:
            finish_nbr_wkld(ns, b, m, lp);
        break;

        case MPI_SEND_ARRIVED:
            update_arrival_queue_rc(ns, b, m, lp);
        break;

        case MPI_SEND_ARRIVED_CB:
            update_message_time_rc(ns, b, m, lp);
        break;

        case MPI_SEND_POSTED:
        {
            if(b->c29)
                handle_next_operation_rc(ns, lp);
            if(b->c28)
                update_completed_queue_rc(ns, b, m, lp);
        }
        break;

        case MPI_REND_ACK_ARRIVED:
        {
            codes_exec_mpi_send_rc(ns, b, m, lp);
        }
        break;

        case MPI_REND_ARRIVED:
        {
            if(b->c10)
                handle_next_operation_rc(ns, lp);

            if(b->c8)
                update_completed_queue_rc(ns, b, m, lp);
            
            ns->recv_time = m->rc.saved_recv_time;
        }
        break;

    default:
         assert(0);
    }
}

static void test_checkpoint_init(
        struct test_checkpoint_state * ns,
        tw_lp * lp)
{ 
    ns->op_status_ct = 0;
    ns->error_ct = 0;
    ns->num_sent_wr = 0;
    ns->num_sent_rd = 0;
    ns->delayed_time = 0.0;
    ns->total_write_time = 0.0;
    ns->total_read_time = 0.0;
    ns->completion_time = 0;
    ns->is_finished = 0;
    ns->num_bursts = 0;

    ns->num_reads = 0;
    ns->num_writes = 0;
    ns->read_size = 0;
    ns->write_size = 0;
    ns->syn_data_sz = 0;
    ns->gen_data_sz = 0;
    ns->random_bb_node_id = -1;
    ns->neighbor_completed = 0;
    ns->max_time = 0;

    INIT_CODES_CB_INFO(&ns->cb, struct test_checkpoint_msg, h, tag, ret);

    ns->cli_rel_id = codes_mapping_get_lp_relative_id(lp->gid, 0, 0);

    codes_mapping_get_lp_info(lp->gid, lp_grp_name, &mapping_gid, lp_name, &mapping_tid, NULL, &mapping_rid, &mapping_offset);

    struct codes_jobmap_id jid;
    jid = codes_jobmap_to_local_id(ns->cli_rel_id, jobmap_ctx);
    if(jid.job == -1)
    {
        ns->app_id = -1;
        ns->local_rank = -1;
        return;
    }

    ns->app_id = jid.job;
    ns->local_rank = jid.rank;
    assert(jid.job < MAX_JOBS);

    /* For communication events */
    INIT_QLIST_HEAD(&ns->arrival_queue);
    INIT_QLIST_HEAD(&ns->pending_recvs_queue);
    INIT_QLIST_HEAD(&ns->completed_reqs);
    INIT_QLIST_HEAD(&ns->msg_sz_list);
    ns->msg_sz_table = NULL;

    /* Initialize the RC stack */
    rc_stack_create(&ns->processed_ops);
    rc_stack_create(&ns->processed_wait_op);
    rc_stack_create(&ns->matched_reqs);    
    assert(ns->processed_ops != NULL);
    assert(ns->processed_wait_op != NULL);
    assert(ns->matched_reqs != NULL);

    /* clock starts ticking when the first event is processed */
    ns->start_time = tw_now(lp);
    ns->num_bytes_sent = 0;
    ns->num_bytes_recvd = 0;
    ns->compute_time = 0;
    ns->elapsed_time = 0;

    if(strcmp(wkld_type_per_job[jid.job], "synthetic") == 0)
    {
        //printf("\n Rank %ld GID %ld generating synthetic traffic ", ns->cli_rel_id, lp->gid);
        kickoff_synthetic_traffic(ns, lp);
    }
    else if(strcmp(wkld_type_per_job[jid.job], "checkpoint") == 0)
    {
        char* w_params = (char*)&c_params;
        ns->wkld_id = codes_workload_load("checkpoint_io_workload", w_params, 0, ns->cli_rel_id);
        handle_next_operation(ns, lp, codes_local_latency(lp));
    }
    else if(strncmp(wkld_type_per_job[jid.job], "conceptual", 10) == 0 || strcmp(wkld_type_per_job[jid.job], "milc") == 0 || strcmp(wkld_type_per_job[jid.job], "nekbone") == 0)
    {
        online_comm_params oc_params;
        strcpy(oc_params.workload_name, wkld_type_per_job[jid.job]); 
        oc_params.nprocs = num_jobs_per_wkld[jid.job]; 
        params = (char*)&oc_params;
        
        if(XIN_DBG)
            printf("\nappid=%d rank=%d(%d) cli_rel_id=%d", ns->app_id, ns->local_rank, oc_params.nprocs, ns->cli_rel_id);
        ns->wkld_id = codes_workload_load("conc_online_comm_workload", params, ns->app_id, ns->local_rank);
        handle_next_operation(ns, lp, codes_local_latency(lp));
    }
    else
    {
        fprintf(stderr, "\n Invalid job id ");
        assert(0);
    }
}

static void test_checkpoint_finalize(
        struct test_checkpoint_state *ns,
        tw_lp *lp)
{
    /*if (ns->num_complete_wr != num_reqs)
        tw_error(TW_LOC, "num_complete_wr:%d does not match num_reqs:%d\n",
                ns->num_complete_wr, num_reqs);
    if (ns->num_complete_rd != num_reqs)
        tw_error(TW_LOC, "num_complete_rd:%d does not match num_reqs:%d\n",
                ns->num_complete_rd, num_reqs);
    */
   struct codes_jobmap_id jid; 
   jid = codes_jobmap_to_local_id(ns->cli_rel_id, jobmap_ctx); 

   if(strncmp(wkld_type_per_job[jid.job], "conceptual", 10) == 0 || strcmp(wkld_type_per_job[jid.job], "milc") == 0 || strcmp(wkld_type_per_job[jid.job], "nekbone") == 0)
        codes_workload_finalize("conc_online_comm_workload", params, ns->app_id, ns->local_rank); 
    
   if(max_write_time < ns->total_write_time)
        max_write_time = ns->total_write_time;

    total_write_time += ns->total_write_time;

    if(tw_now(lp) - ns->start_time > max_total_time)
        max_total_time = tw_now(lp) - ns->start_time;

    total_run_time += (tw_now(lp) - ns->start_time);
    total_syn_data += ns->syn_data_sz;

    int written = 0;
    double avg_msg_time = 0;
    avg_msg_time = (ns->send_time / ns->num_recvs);

    // int count_irecv = 0, count_isend = 0;
    // count_irecv = qlist_count(&ns->pending_recvs_queue);
    // count_isend = qlist_count(&ns->arrival_queue);
    // printf("\ncount_irecv=%d, count_isend=%d\n",count_irecv, count_isend);

    // if(count_irecv > 0 || count_isend > 0)
    // {
    //     unmatched = 1;
    //     printf("\n nw-id %llu unmatched irecvs %d unmatched sends %d Total sends %ld receives %ld delays %ld wait alls %ld waits %ld send time %lf wait %lf",
    //             LLU(ns->cli_rel_id) , count_irecv, count_isend, ns->num_sends, ns->num_recvs, 
    //             ns->num_delays, ns->num_waitall, ns->num_wait, ns->send_time, ns->wait_time);
    // }

    // if(count_irecv || count_isend)
    // {
    //     print_msgs_queue(&ns->pending_recvs_queue, 0);
    //     print_msgs_queue(&ns->arrival_queue, 1);
    // }
    if(ns->cli_rel_id == TRACK_LP)
        printf("\n Job %d Rank %d Outputing log files", ns->app_id, ns->local_rank);

    written = 0;
    if(!ns->cli_rel_id)
        written = sprintf(ns->output_buf, "# Format <LP ID> <Terminal ID> <Job ID> <Local Rank> <Total sends> <Total Recvs> <Bytes sent> <Bytes recvd> <Send time> <Comm. time> <Compute time> <Avg msg time> <Max Msg Time>");
    written += sprintf(ns->output_buf + written, "\n%llu %llu %d %d %ld %ld %llu %llu %lf %lf %lf %lf %lf", 
                        LLU(lp->gid), LLU(ns->cli_rel_id), ns->app_id, ns->local_rank, ns->num_sends, ns->num_recvs, 
                        ns->num_bytes_sent, ns->num_bytes_recvd, ns->send_time, ns->elapsed_time - ns->compute_time, 
                        ns->compute_time, avg_msg_time, ns->elapsed_time);
    lp_io_write(lp->gid, (char*)"mpi-replay-stats", written, ns->output_buf);

    written = 0;
    if(!ns->cli_rel_id)
        written = sprintf(ns->output_buf1, "# Format <LP id> <Workload type> <client id> <Bytes written> <Bytes read> <Synthetic data Received> <Time to write bytes > <Total elapsed time>");
   
    written += sprintf(ns->output_buf1 + written, "\n%"PRId64" %s %d %llu %llu %"PRId64" %lf %lf", lp->gid, wkld_type_per_job[jid.job], 
                        ns->cli_rel_id, ns->write_size, ns->read_size, ns->syn_data_sz, ns->total_write_time, tw_now(lp) - ns->start_time);
    lp_io_write(lp->gid, "checkpoint-client-stats", written, ns->output_buf1);   

    tw_stime my_comm_time = ns->elapsed_time - ns->compute_time;
    if(my_comm_time > max_comm_time)
        max_comm_time = my_comm_time;

    if(ns->elapsed_time > max_elapsed_time_per_job[ns->app_id])
        max_elapsed_time_per_job[ns->app_id] = ns->elapsed_time;

    if(ns->elapsed_time > max_time )
        max_time = ns->elapsed_time;

    if(ns->wait_time > max_wait_time)
        max_wait_time = ns->wait_time;
    
    if(ns->send_time > max_send_time)
        max_send_time = ns->send_time;

    if(ns->recv_time > max_recv_time)
        max_recv_time = ns->recv_time;

    avg_time += ns->elapsed_time;
    avg_comm_time += (ns->elapsed_time - ns->compute_time);
    avg_wait_time += ns->wait_time;
    avg_send_time += ns->send_time;
    avg_recv_time += ns->recv_time;

    // rc_stack_destroy(ns->matched_reqs);
    // rc_stack_destroy(ns->processed_ops);
    // rc_stack_destroy(ns->processed_wait_op);
}

tw_lptype test_checkpoint_lp = {
    (init_f) test_checkpoint_init,
    (pre_run_f) NULL,
    (event_f) test_checkpoint_event,
    (revent_f) test_checkpoint_event_rc,
    (commit_f) NULL,
    (final_f) test_checkpoint_finalize,
    (map_f) codes_mapping,
    sizeof(struct test_checkpoint_state),
};

void test_checkpoint_register(){
    lp_type_register(CHK_LP_NM, &test_checkpoint_lp);
}

void test_checkpoint_configure(int model_net_id){
    uint32_t h1=0, h2=0;

    bj_hashlittle2(CHK_LP_NM, strlen(CHK_LP_NM), &h1, &h2);
    test_checkpoint_magic = h1+h2;
    cli_dfly_id = model_net_id;

    int rc;
    rc = configuration_get_value_double(&config, "test-checkpoint-client", "checkpoint_sz", NULL,
	   &c_params.checkpoint_sz);

    assert(!rc);

    rc = configuration_get_value_double(&config, "test-checkpoint-client", "checkpoint_wr_bw", NULL,
	   &c_params.checkpoint_wr_bw);
    assert(!rc);

    rc = configuration_get_value_int(&config, "test-checkpoint-client", "chkpoint_iters", NULL,
	   &total_checkpoints);
    assert(!rc);
    c_params.total_checkpoints = total_checkpoints;

    rc = configuration_get_value_double(&config, "test-checkpoint-client", "mtti", NULL,
	   &c_params.mtti);
    assert(!rc);
   
    num_servers =
        codes_mapping_get_lp_count(NULL, 0, CODES_STORE_LP_NAME, NULL, 1);
    num_clients = 
        codes_mapping_get_lp_count(NULL, 0, CHK_LP_NM, NULL, 1);

    c_params.nprocs = num_chk_clients;
    if(num_chk_clients)
    {
        my_checkpoint_sz = terabytes_to_megabytes(c_params.checkpoint_sz)/num_chk_clients;
    }

    clients_per_server = num_clients / num_servers;
    printf("\nNumber of clients per server %d", clients_per_server);
    if (clients_per_server == 0)
        clients_per_server = 1;
}

const tw_optdef app_opt[] = {
    TWOPT_GROUP("codes-store mock test model"),
    TWOPT_CHAR("workload-conf-file", workloads_conf_file, "Workload allocation of participating ranks "),
    TWOPT_CHAR("rank-alloc-file", alloc_file, "detailed rank allocation, which rank gets what workload "),
    TWOPT_CHAR("codes-config", conf_file_name, "Name of codes configuration file"),
    TWOPT_CHAR("lp-io-dir", lp_io_dir, "Where to place io output (unspecified -> no output"),
    TWOPT_UINT("lp-io-use-suffix", lp_io_use_suffix, "Whether to append uniq suffix to lp-io directory (default 0)"),
    TWOPT_UINT("random-bb", random_bb_nodes, "Whether use randomly selected burst buffer nodes or nearby nodes"),
    TWOPT_UINT("max-gen-data", max_gen_data, "Maximum data to generate "),
    TWOPT_UINT("payload-sz", PAYLOAD_SZ, "the payload size for uniform random traffic"),
    TWOPT_END()
};

void Usage()
{
   if(tw_ismaster())
    {
        fprintf(stderr, "\n mpirun -np n ./client-mul-wklds --sync=1/3"
                "--workload-conf-file = workload-conf-file --alloc_file = alloc-file"
                "--codes-config=codes-config-file\n");
    }
}

int main(int argc, char * argv[])
{

    MPI_Init(&argc,&argv);
    ABT_init(argc, argv);

    int rank, nprocs;
    int num_nets, *net_ids;
    int model_net_id;
    int simple_net_id;
    int num_total_jobs;

    g_tw_ts_end = s_to_ns(60*60*24*365); /* one year, in nsecs */

    tw_opt_add(app_opt);
    tw_init(&argc, &argv);

    if(strlen(workloads_conf_file) == 0 || strlen(alloc_file) == 0 || !conf_file_name[0])
    {
        Usage();
        MPI_Finalize();
        return 1;
    }

    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &nprocs);

    /* This file will have the names of workloads and the number of
     * ranks that should be allocated to each of those workloads */
    FILE * wkld_info_file = fopen(workloads_conf_file, "r+");
    assert(wkld_info_file);

    int i =0;
    char separator = "\n";
    while(!feof(wkld_info_file))
    {
        separator = fscanf(wkld_info_file, "%d %s", &num_jobs_per_wkld[i], wkld_type_per_job[i]);
        
        if(strcmp(wkld_type_per_job[i], "synthetic") == 0)
            num_syn_clients = num_jobs_per_wkld[i];

        if(strcmp(wkld_type_per_job[i], "checkpoint") == 0 || strncmp(wkld_type_per_job[i], "conceptual", 10) == 0 || strcmp(wkld_type_per_job[i], "milc") == 0 || strcmp(wkld_type_per_job[i], "nekbone") == 0)
            num_chk_clients += num_jobs_per_wkld[i];

        if(separator != EOF)
        {
            printf("\n%d instances of workload %s \n", num_jobs_per_wkld[i], wkld_type_per_job[i]);
            i++;
        }
    }
    /* loading the config file into the codes-mapping utility, giving us the
     * parsed config object in return. 
     * "config" is a global var defined by codes-mapping */
    if (configuration_load(conf_file_name, MPI_COMM_WORLD, &config)){
        fprintf(stderr, "Error loading config file %s.\n", conf_file_name);
        MPI_Finalize();
        return 1;
    }

    /* Now assign the alloc file to job map */
    jobmap_p.alloc_file = alloc_file;
    jobmap_ctx = codes_jobmap_configure(CODES_JOBMAP_LIST, &jobmap_p);
    num_total_jobs = codes_jobmap_get_num_jobs(jobmap_ctx);

    // register lps
    lsm_register();
    codes_store_register();
    resource_lp_init();
    test_checkpoint_register();
    // test_dummy_register();
    codes_ex_store_register();
    model_net_register();

    /* model-net enable sampling */
    // model_net_enable_sampling(500000000, 1500000000000);

    /* Setup takes the global config object, the registered LPs, and 
     * generates/places the LPs as specified in the configuration file. 
     * This should only be called after ALL LP types have been registered in 
     * codes */
    codes_mapping_setup();
    // congestion_control_set_jobmap(jobmap_ctx, cli_dfly_id); 

    /* Setup the model-net parameters specified in the global config object,
     * returned is the identifier for the network type */
    net_ids = model_net_configure(&num_nets);
    
    /* Network topology supported is either only simplenet
     * OR dragonfly */
    model_net_id = net_ids[0];
 
    if(net_ids[0] == DRAGONFLY_CUSTOM || net_ids[0] == DRAGONFLY_DALLY)
       simple_net_id = net_ids[2];
    
    free(net_ids);

    /* after the mapping configuration is loaded, let LPs parse the
     * configuration information. This is done so that LPs have access to
     * the codes_mapping interface for getting LP counts and such */
    codes_store_configure(model_net_id);
    
    if(num_nets > 1)
       codes_store_set_scnd_net(simple_net_id);
   
    resource_lp_configure();
    lsm_configure();
    test_checkpoint_configure(model_net_id);

    if (lp_io_dir[0]){
        do_lp_io = 1;
        /* initialize lp io */
        int flags = lp_io_use_suffix ? LP_IO_UNIQ_SUFFIX : 0;
        int ret = lp_io_prepare(lp_io_dir, flags, &io_handle, MPI_COMM_WORLD);
        assert(ret == 0 || !"lp_io_prepare failure");
    }

    //Output iteration time in log file
    char tmploc[MAX_NAME_LENGTH]; 
    strcpy(tmploc,io_handle);
    strcat(tmploc,"/iteration-logs");
    iteration_log = fopen(tmploc, "w+");
    if(!iteration_log)
    {
       printf("\nError logging iteration times... quitting ");
       MPI_Finalize();
       return -1;
    }
    if(enable_wkld_log)
    {
        workload_log = fopen("mpi-op-logs", "w+");

        if(!workload_log)
        {
            printf("\nError logging MPI operations... quitting ");
            MPI_Finalize();
            return -1;
        }
    }

    if(!g_tw_mynode)
    {
        // char meta_file[64];
        // sprintf(meta_file, "checkpoint-client-stats.meta");
        
        // FILE * fp = fopen(meta_file, "w+");
        // fprintf(fp, "# Format <LP id> <Workload type> <client id> <Bytes written> <Bytes read> <Synthetic data received> <Time to write bytes> <Total time elapsed>");
        // fclose(fp);

        printf("\nMy checkpoint sz %lf MiB", my_checkpoint_sz);
    }

    switch(map_ctxt)
    {
        case GROUP_RATIO:
           mapping_context = codes_mctx_set_group_ratio(NULL, true);
           break;
        case GROUP_RATIO_REVERSE:
           mapping_context = codes_mctx_set_group_ratio_reverse(NULL, true);
           break;
        case GROUP_DIRECT:
           mapping_context = codes_mctx_set_group_direct(1,NULL, true);
           break;
        case GROUP_MODULO:
           mapping_context = codes_mctx_set_group_modulo(NULL, true);
           break;
        case GROUP_MODULO_REVERSE:
           mapping_context = codes_mctx_set_group_modulo_reverse(NULL, true);
           break;
    }

    tw_run();

    fclose(iteration_log);

    if(enable_wkld_log)
        fclose(workload_log);

    double g_max_total_time, g_max_write_time, g_avg_time, g_avg_write_time, g_total_syn_data;
    MPI_Reduce(&max_total_time, &g_max_total_time, 1, MPI_DOUBLE, MPI_MAX, 0, MPI_COMM_WORLD);  
    MPI_Reduce(&max_write_time, &g_max_write_time, 1, MPI_DOUBLE, MPI_MAX, 0, MPI_COMM_WORLD);  
    MPI_Reduce(&total_write_time, &g_avg_write_time, 1, MPI_DOUBLE, MPI_SUM, 0, MPI_COMM_WORLD);  
    MPI_Reduce(&total_syn_data, &g_total_syn_data, 1, MPI_DOUBLE, MPI_SUM, 0, MPI_COMM_WORLD);  
    MPI_Reduce(&total_run_time, &g_avg_time, 1, MPI_DOUBLE, MPI_SUM, 0, MPI_COMM_WORLD);  


    long long total_bytes_sent, total_bytes_recvd;
    double max_run_time, avg_run_time;
    double max_comm_run_time, avg_comm_run_time;
    double total_avg_send_time, total_max_send_time;
    double total_avg_wait_time, total_max_wait_time;
    double total_avg_recv_time, total_max_recv_time;

    MPI_Reduce(&num_bytes_sent, &total_bytes_sent, 1, MPI_LONG_LONG, MPI_SUM, 0, MPI_COMM_CODES);
    MPI_Reduce(&num_bytes_recvd, &total_bytes_recvd, 1, MPI_LONG_LONG, MPI_SUM, 0, MPI_COMM_CODES);
    MPI_Reduce(&max_comm_time, &max_comm_run_time, 1, MPI_DOUBLE, MPI_MAX, 0, MPI_COMM_CODES);
    MPI_Reduce(&max_time, &max_run_time, 1, MPI_DOUBLE, MPI_MAX, 0, MPI_COMM_CODES);
    MPI_Reduce(&avg_time, &avg_run_time, 1, MPI_DOUBLE, MPI_SUM, 0, MPI_COMM_CODES);
    MPI_Reduce(&avg_recv_time, &total_avg_recv_time, 1, MPI_DOUBLE, MPI_SUM, 0, MPI_COMM_CODES);
    MPI_Reduce(&avg_comm_time, &avg_comm_run_time, 1, MPI_DOUBLE, MPI_SUM, 0, MPI_COMM_CODES);
    MPI_Reduce(&max_wait_time, &total_max_wait_time, 1, MPI_DOUBLE, MPI_MAX, 0, MPI_COMM_CODES);
    MPI_Reduce(&max_send_time, &total_max_send_time, 1, MPI_DOUBLE, MPI_MAX, 0, MPI_COMM_CODES);
    MPI_Reduce(&max_recv_time, &total_max_recv_time, 1, MPI_DOUBLE, MPI_MAX, 0, MPI_COMM_CODES);
    MPI_Reduce(&avg_wait_time, &total_avg_wait_time, 1, MPI_DOUBLE, MPI_SUM, 0, MPI_COMM_CODES);
    MPI_Reduce(&avg_send_time, &total_avg_send_time, 1, MPI_DOUBLE, MPI_SUM, 0, MPI_COMM_CODES);

    assert(num_chk_clients);

    if(!g_tw_mynode)
    {
        printf("\n Maximum time spent by compute nodes %lf Maximum time spent in write operations %lf", g_max_total_time, g_max_write_time);
        printf("\n Avg time spent by compute nodes %lf Avg time spent in write operations %lf\n", g_avg_time/num_clients, g_avg_write_time/num_chk_clients);

        printf("\n Synthetic traffic stats: data received per proc %lf ", g_total_syn_data/num_syn_clients);
        
        printf("\n ----------\n");
        printf("\n Total bytes sent %lld recvd %lld \n max runtime %lf ns avg runtime %lf \n max comm time %lf avg comm time %lf \n max send time %lf avg send time %lf \n max recv time %lf avg recv time %lf \n max wait time %lf avg wait time %lf \n", 
                total_bytes_sent, total_bytes_recvd,
                max_run_time, avg_run_time/num_chk_clients,
                max_comm_run_time, avg_comm_run_time/num_chk_clients,
                total_max_send_time, total_avg_send_time/num_chk_clients,
                total_max_recv_time, total_avg_recv_time/num_chk_clients,
                total_max_wait_time, total_avg_wait_time/num_chk_clients);

        printf("\n ----------\n");
        printf(" Per App Max Elapsed Times:\n");
        for(int i = 0; i < num_total_jobs; i++)
        {
            printf("\tApp %d: %.4f\n", i, max_elapsed_time_per_job[i]);
        }
        printf("\n ----------\n");
    }
    if (do_lp_io){
        int ret = lp_io_flush(io_handle, MPI_COMM_WORLD);
        assert(ret == 0 || !"lp_io_flush failure");
    }

    model_net_report_stats(model_net_id);
    if(unmatched && g_tw_mynode == 0) 
       fprintf(stderr, "\n Warning: unmatched send and receive operations found.\n");

    codes_jobmap_destroy(jobmap_ctx);
    tw_end();

    ABT_finalize();    

    int flag;
    MPI_Finalized(&flag);
    if(!flag) MPI_Finalize();  

    return 0;
}
/*
 * Local variables:
 *  c-indent-level: 4
 *  c-basic-offset: 4
 *  indent-tabs-mode: nil
 * End:
 *
 * vim: ft=c ts=8 sts=4 sw=4 expandtab
 */
