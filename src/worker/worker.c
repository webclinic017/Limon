#include "worker_internal.h"
#include "kverrno.h"
#include "slab.h"
#include "rbtree_uint.h"
#include "kvutil.h"

#include "spdk/log.h"

//10s
#define DEFAULT_RECLAIM_POLLING_PERIOD_US 10000000

static void 
_process_one_kv_request(struct worker_context *wctx, struct kv_request_internal *req){
    if(!req->pctx.no_lookup){
        req->pctx.entry  = mem_index_lookup(wctx->mem_index,req->item);
    }
    req->pctx.wctx = wctx;
    req->pctx.slab = NULL;
    req->pctx.old_slab = NULL;

    switch(req->op_code){
        case GET:
            worker_process_get(req);
            break;
        case PUT:
            worker_process_put(req);
            break;
        case DELETE:
            worker_process_delete(req);
            break;
        case FIRST:
            worker_process_first(req);
            break;
        case SEEK:
            worker_process_seek(req);
            break;
        case NEXT:
            worker_process_next(req);
            break;
        default:
            pool_release(wctx->kv_request_internal_pool,req);
            req->cb_fn(req->ctx,NULL,-KV_EOP_UNKNOWN);
            break;
    }
}

static int
_worker_business_processor_poll(void*ctx){
    struct worker_context *wctx = ctx;
    int events = 0;

    // The pending requests in the worker context no-lock request queue
    uint32_t p_reqs = spdk_ring_count(wctx->req_used_ring);

    // The avalaible pending requests in the worker request pool
    uint32_t a_reqs = wctx->kv_request_internal_pool->nb_frees;

    //The avalaible disk io.
    uint32_t a_ios = (wctx->imgr->max_pending_io < wctx->imgr->nb_pending_io) ? 0 :
                     (wctx->imgr->max_pending_io - wctx->imgr->nb_pending_io);

    /**
     * @brief Get the minimum value from the rquests queue, kv request internal pool
     * and disk io manager
     */
    uint32_t process_size = p_reqs < a_reqs ? p_reqs : a_reqs;
    process_size = process_size < a_ios ? process_size : a_ios;

    //SPDK_NOTICELOG("p_reqs:%u, a_reqs:%u, a_ios:%u, process:%u\n",p_reqs,a_reqs, a_ios, process_size);

    uint32_t i = 0;
    struct kv_request_internal *req_internal,*tmp=NULL;
    struct kv_request *req;

    /**
     * @brief Process the resubmit queue. I needn't get new cached requests
     * from requests pool, since requests from resubmit queue already have 
     * cached request object.
     * Just link then to submit queue.
     */
    TAILQ_FOREACH_SAFE(req_internal,&wctx->resubmit_queue,link,tmp){
            TAILQ_REMOVE(&wctx->resubmit_queue,req_internal,link);
            TAILQ_INSERT_HEAD(&wctx->submit_queue,req_internal,link);
    }

    /**
     * @brief Process the submit queue. For every kv request from users, I have to
     * get a request_internal object from request object pool to wrap it.
     */
    if(process_size > 0){
        for(;i<process_size;i++){
            uint64_t res;
            res = spdk_ring_dequeue(wctx->req_used_ring,(void**)&req,1);
            assert(res==1);

            //For test
            req->cb_fn(req->ctx,NULL,0);
            res = spdk_ring_enqueue(wctx->req_free_ring,(void**)&req,1,NULL);
            assert(res==1);

            return;

            req_internal = pool_get(wctx->kv_request_internal_pool);
            assert(req_internal!=NULL);

            req_internal->ctx = req->ctx;
            req_internal->item = req->item;
            req_internal->op_code = req->op_code;
            req_internal->cb_fn = req->cb_fn;
            req_internal->shard = req->shard;

            //I do not perform a lookup, since it is a new request.
            req_internal->pctx.no_lookup = false;

            TAILQ_INSERT_TAIL(&wctx->submit_queue,req_internal,link);

            res = spdk_ring_enqueue(wctx->req_free_ring,(void**)&req,1,NULL);
            assert(res==1);
        }
    }
    TAILQ_FOREACH_SAFE(req_internal, &wctx->submit_queue,link,tmp){
        TAILQ_REMOVE(&wctx->submit_queue,req_internal,link);
        _process_one_kv_request(wctx, req_internal);
        events++;
    }

    return events;
}

static int
_worker_reclaim_poll(void* ctx){
    struct worker_context *wctx = ctx;
    int events = 0;
    events += worker_reclaim_process_pending_slab_migrate(wctx);
    events += worker_reclaim_process_pending_item_migrate(wctx);
    events += worker_reclaim_process_pending_item_delete(wctx);
    return events;
}

static void
_fill_slab_migrate_req(struct slab_migrate_request *req, struct slab* slab ){
    //Submit a slab shrinking request
    req->slab = slab;
    req->is_fault = false;
    req->node = rbtree_last(slab->reclaim.total_tree);

    req->nb_processed = 0;
    req->start_slot = slab_reclaim_get_start_slot(&slab->reclaim,req->node);
    req->cur_slot = req->start_slot;
    req->last_slot = slab->reclaim.nb_total_slots-1;

    if(!rbtree_lookup(slab->reclaim.free_node_tree,slab->reclaim.nb_reclaim_nodes-1)){
        //The last reclaim node is not allowed to performing allocating.  
        rbtree_delete(slab->reclaim.free_node_tree,slab->reclaim.nb_reclaim_nodes-1,NULL);
    }
}
//This poller shall be excecuted regularly, e.g. once a second.
static int
_worker_slab_evaluation_poll(void* ctx){
    struct worker_context *wctx = ctx;
    uint32_t i=0,j=0;

    int events = 0;

    for(;i < wctx->nb_reclaim_shards;i++){
        struct slab_shard *shard = &wctx->shards[wctx->reclaim_shards_start_id + i];
        for(;j < shard->nb_slabs;j++){
            if( !(shard->slab_set[j].flag | SLAB_FLAG_RECLAIMING) ){
                struct slab* slab = &(shard->slab_set[j]);
                if( slab_reclaim_evaluate_slab(slab) ){
                    struct slab_migrate_request *slab_migrate_req = pool_get(wctx->rmgr->migrate_slab_pool);
                    assert(slab_migrate_req!=NULL);

                    _fill_slab_migrate_req(slab_migrate_req,slab);
                    slab->flag |= SLAB_FLAG_RECLAIMING;
                    TAILQ_INSERT_TAIL(&wctx->rmgr->slab_migrate_head,slab_migrate_req,link);

                    events++;
                }
            }
        }
    }
    return events;
}

// Get a slot buffer from woker request queue.
static uint32_t
get_request_buffer(struct worker_context *wctx) {
    uint32_t next_buffer = atomic_fetch_add(&wctx->buffered_request_idx,1);
    while(1) {
        uint32_t pending = next_buffer - atomic_load(&wctx->processed_requests);
        if(pending >= wctx->max_pending_kv_request) { // Queue is full, wait
            NOP10();
        } else {
            break;
        }
    }
    return next_buffer % wctx->max_pending_kv_request;
}

// Once we get a slot, we fill it, and then submit it */
static uint32_t
submit_request_buffer(struct worker_context *wctx, uint32_t buffer_idx) {
    while(1) {
        uint32_t sr = atomic_load(&wctx->sent_requests);
        if(sr%wctx->max_pending_kv_request != buffer_idx) { 
        // Somebody else is enqueuing a request, wait!
            NOP10();
        } else {
            break;
        }
    }
    return atomic_fetch_add(&wctx->sent_requests, 1);
}

static struct kv_request*
_get_free_req_buffer(struct worker_context* wctx){
    struct kv_request *req;
    while(spdk_ring_dequeue(wctx->req_free_ring,(void**)&req,1)==0){
        //No free kv_request are available.
        //Wait here.
        spdk_delay_us(10);
    }
    return req;
}

static void
_submit_req_buffer(struct worker_context* wctx,struct kv_request *req){
    uint64_t res;
    res = spdk_ring_enqueue(wctx->req_used_ring,(void**)&req,1,NULL);
    assert(res==1);
}

static inline void
_worker_enqueue_common(struct kv_request* req, uint32_t shard,struct kv_item *item, 
                       worker_cb cb_fn,void* ctx,
                       enum op_code op){
    req->cb_fn = cb_fn;
    req->ctx = ctx;
    req->item = item;
    req->op_code = op;
    req->shard = shard;
}

void worker_enqueue_get(struct worker_context* wctx,uint32_t shard,struct kv_item *item, 
                        worker_cb cb_fn, void* ctx){

    //unsigned int buffer_slot  = get_request_buffer(wctx);
    struct kv_request *req = _get_free_req_buffer(wctx);
    _worker_enqueue_common(req,shard,item,cb_fn,ctx,GET);
    _submit_req_buffer(wctx,req);
    //submit_request_buffer(wctx,buffer_slot);
}

void worker_enqueue_put(struct worker_context* wctx,uint32_t shard,struct kv_item *item,
                        worker_cb cb_fn, void* ctx){
    //unsigned int buffer_slot  = get_request_buffer(wctx);
    struct kv_request *req = _get_free_req_buffer(wctx);
    _worker_enqueue_common(req,shard,item,cb_fn,ctx,PUT);
    _submit_req_buffer(wctx,req);
    //submit_request_buffer(wctx,buffer_slot);
}

void worker_enqueue_delete(struct worker_context* wctx,uint32_t shard,struct kv_item *item, 
                           worker_cb cb_fn, void* ctx){
    //unsigned int buffer_slot  = get_request_buffer(wctx);
    struct kv_request *req = _get_free_req_buffer(wctx);
    _worker_enqueue_common(req,shard,item,cb_fn,ctx,DELETE);
    _submit_req_buffer(wctx,req);
    //submit_request_buffer(wctx,buffer_slot);
}

void worker_enqueue_first(struct worker_context* wctx,uint32_t shard,struct kv_item *item,
                         worker_cb cb_fn, void* ctx){
   // unsigned int buffer_slot  = get_request_buffer(wctx);
    struct kv_request *req = _get_free_req_buffer(wctx);
    _worker_enqueue_common(req,shard,item,cb_fn,ctx,FIRST);
    _submit_req_buffer(wctx,req);
    //submit_request_buffer(wctx,buffer_slot);
}

void worker_enqueue_seek(struct worker_context* wctx,uint32_t shard,struct kv_item *item,
                         worker_cb cb_fn, void* ctx){
    //unsigned int buffer_slot  = get_request_buffer(wctx);
    struct kv_request *req = _get_free_req_buffer(wctx);
    _worker_enqueue_common(req,shard,item,cb_fn,ctx,SEEK);
    _submit_req_buffer(wctx,req);
    //submit_request_buffer(wctx,buffer_slot);
}

void worker_enqueue_next(struct worker_context* wctx,uint32_t shard,struct kv_item *item, 
                         worker_cb cb_fn, void* ctx){
    //unsigned int buffer_slot  = get_request_buffer(wctx);
    struct kv_request *req = _get_free_req_buffer(wctx);
    _worker_enqueue_common(req,shard,item,cb_fn,ctx,NEXT);
    _submit_req_buffer(wctx,req);
    //submit_request_buffer(wctx,buffer_slot);
}

static void
_worker_init_pmgr(struct pagechunk_mgr* pmgr,struct worker_init_opts* opts){
    uint64_t nb_max_reqs = opts->max_request_queue_size_per_worker*5;
    uint64_t common_header_size = pool_header_size(nb_max_reqs);
    uint64_t size;

    pmgr->hit_times = 0;
    pmgr->miss_times = 0;
    pmgr->nb_used_chunks = 0;
    TAILQ_INIT(&pmgr->global_chunks);
    pmgr->chunkmgr_worker = opts->chunkmgr_worker;

    pmgr->kv_chunk_request_pool = (struct object_cache_pool*)(pmgr+1);
    uint8_t* req_pool_data = (uint8_t*)pmgr->kv_chunk_request_pool + common_header_size;

    assert((uint64_t)pmgr->kv_chunk_request_pool%8==0);
    assert((uint64_t)req_pool_data%8==0);
    size = pool_header_init(pmgr->kv_chunk_request_pool,nb_max_reqs,sizeof(struct chunk_miss_callback),
                     common_header_size,
                     req_pool_data);

    assert(size%8==0);
    pmgr->load_store_ctx_pool = (struct object_cache_pool*)((uint8_t*)pmgr->kv_chunk_request_pool+size);
    uint8_t* ctx_pool_data = (uint8_t*)pmgr->load_store_ctx_pool + common_header_size;
    
    assert((uint64_t)pmgr->load_store_ctx_pool%8==0);
    assert((uint64_t)ctx_pool_data%8==0);
    pool_header_init(pmgr->load_store_ctx_pool,nb_max_reqs,sizeof(struct chunk_load_store_ctx),
                     common_header_size,
                     ctx_pool_data);
}

static void
_worker_init_rmgr(struct reclaim_mgr* rmgr,struct worker_init_opts* opts){

    uint64_t nb_max_reqs = opts->max_request_queue_size_per_worker;
    uint32_t nb_max_slab_reclaim = opts->nb_reclaim_shards*opts->shard[0].nb_slabs;
    uint64_t del_header_size = pool_header_size(nb_max_reqs*3);
    uint64_t mig_header_size = pool_header_size(nb_max_reqs);
    uint64_t size;

    rmgr->migrating_batch = opts->reclaim_batch_size;
    rmgr->reclaim_percentage_threshold = opts->reclaim_percentage_threshold;
    TAILQ_INIT(&rmgr->item_delete_head);
    TAILQ_INIT(&rmgr->item_migrate_head);
    TAILQ_INIT(&rmgr->remigrating_head);
    TAILQ_INIT(&rmgr->slab_migrate_head);

    rmgr->pending_delete_pool = (struct object_cache_pool*)(rmgr+1);
    uint8_t *del_pool_data = (uint8_t*)rmgr->pending_delete_pool + del_header_size;

    assert((uint64_t)rmgr->pending_delete_pool%8==0);
    assert((uint64_t)del_pool_data%8==0);
    size = pool_header_init(rmgr->pending_delete_pool,nb_max_reqs*3,sizeof(struct pending_item_delete),
                     del_header_size,
                     del_pool_data);

    assert(size%8==0);
    rmgr->pending_migrate_pool = (struct object_cache_pool*)((uint8_t*)rmgr->pending_delete_pool + size);
    uint8_t* mig_pool_data = (uint8_t*)rmgr->pending_migrate_pool + mig_header_size;

    assert((uint64_t)rmgr->pending_migrate_pool%8==0);
    assert((uint64_t)mig_pool_data%8==0);
    size = pool_header_init(rmgr->pending_migrate_pool,nb_max_reqs,sizeof(struct pending_item_migrate),
                            mig_header_size,
                            mig_pool_data);

    assert(size%8==0);            
    rmgr->migrate_slab_pool = (struct object_cache_pool*)((uint8_t*)rmgr->pending_migrate_pool + size);
    uint8_t* slab_pool_data = (uint8_t*)rmgr->migrate_slab_pool + pool_header_size(nb_max_slab_reclaim);

    assert((uint64_t)rmgr->migrate_slab_pool%8==0);
    assert((uint64_t)slab_pool_data%8==0);
    pool_header_init(rmgr->migrate_slab_pool,nb_max_slab_reclaim,sizeof(struct slab_migrate_request),
                     pool_header_size(nb_max_slab_reclaim),
                     slab_pool_data);
}

static void
_worker_init_imgr(struct iomgr* imgr,struct worker_init_opts* opts){
    uint64_t nb_max_reqs = opts->max_request_queue_size_per_worker;
    uint64_t cio_header_size = pool_header_size(nb_max_reqs*10);
    uint64_t pio_header_size = pool_header_size(nb_max_reqs*20);
    uint64_t size;

    imgr->meta_thread = opts->meta_thread;
    imgr->max_pending_io = opts->max_io_pending_queue_size_per_worker;
    imgr->nb_pending_io = 0;
    imgr->read_hash.cache_hash = NULL;
    imgr->read_hash.page_hash = NULL;
    imgr->write_hash.cache_hash = NULL;
    imgr->write_hash.page_hash = NULL;

    imgr->cache_io_pool = (struct object_cache_pool*)(imgr+1);
    uint8_t* cahe_pool_data = (uint8_t*)imgr->cache_io_pool + cio_header_size;

    assert((uint64_t)imgr->cache_io_pool%8==0);
    assert((uint64_t)cahe_pool_data%8==0);
    size = pool_header_init(imgr->cache_io_pool,nb_max_reqs*10,sizeof(struct cache_io),
                     cio_header_size,
                     cahe_pool_data);

    assert(size%8==0);
    imgr->page_io_pool = (struct object_cache_pool*)((uint8_t*)imgr->cache_io_pool + size);
    uint8_t* page_pool_data = (uint8_t*)imgr->page_io_pool + pio_header_size;

    assert((uint64_t)imgr->page_io_pool%8==0);
    assert((uint64_t)page_pool_data%8==0);
    pool_header_init(imgr->page_io_pool,nb_max_reqs*20,sizeof(struct page_io),
                     pio_header_size,
                     page_pool_data);
}

static void
_worker_context_init(struct worker_context *wctx,struct worker_init_opts* opts,
                    uint64_t pmgr_base_off,
                    uint64_t rmgr_base_off,
                    uint64_t imgr_base_off){

    uint32_t nb_max_reqs = opts->max_request_queue_size_per_worker;

    wctx->mem_index = mem_index_init();
    wctx->shards = opts->shard;
    wctx->nb_shards = opts->nb_shards;
    wctx->nb_reclaim_shards = opts->nb_reclaim_shards;
    wctx->reclaim_shards_start_id = opts->reclaim_shard_start_id;
    wctx->reclaim_percentage_threshold = opts->reclaim_percentage_threshold;
    wctx->request_queue = (struct kv_request*)(wctx + 1);
    wctx->buffered_request_idx = 0;
    wctx->sent_requests = 0;
    wctx->processed_requests = 0;
    wctx->max_pending_kv_request = nb_max_reqs;
    
    wctx->req_used_ring = spdk_ring_create(SPDK_RING_TYPE_MP_SC,nb_max_reqs,SPDK_ENV_SOCKET_ID_ANY);
    wctx->req_free_ring = spdk_ring_create(SPDK_RING_TYPE_MP_MC,nb_max_reqs,SPDK_ENV_SOCKET_ID_ANY);
    assert(wctx->req_used_ring!=NULL);
    assert(wctx->req_free_ring!=NULL);

    //Put all free kv_request into req_free_ring.
    uint32_t i = 0;
    struct kv_request* req;
    for(;i<nb_max_reqs;i++){
        req = &wctx->request_queue[i];
        spdk_ring_enqueue(wctx->req_free_ring, (void**)&req,1,NULL);
    }

    TAILQ_INIT(&wctx->submit_queue);
    TAILQ_INIT(&wctx->resubmit_queue);

    wctx->kv_request_internal_pool = (struct object_cache_pool*)(wctx->request_queue + nb_max_reqs);
    uint8_t* req_pool_data = (uint8_t*)wctx->kv_request_internal_pool + pool_header_size(nb_max_reqs);

    assert((uint64_t)wctx->kv_request_internal_pool%8==0);
    assert((uint64_t)req_pool_data%8==0);
    pool_header_init(wctx->kv_request_internal_pool,nb_max_reqs,sizeof(struct kv_request_internal),
                     pool_header_size(nb_max_reqs),
                     req_pool_data);

    struct spdk_cpuset cpumask;
    //The name will be copied. So the stack variable makes sense.
    char thread_name[32];

    spdk_cpuset_zero(&cpumask);
    spdk_cpuset_set_cpu(&cpumask,opts->core_id,true);
    snprintf(thread_name,sizeof(thread_name),"worker_%u",opts->core_id);
    wctx->target = opts->target;
    wctx->thread = spdk_thread_create(thread_name,&cpumask);
    //wctx->thread = spdk_thread_create(thread_name,NULL);
    assert(wctx->thread!=NULL);

    assert(pmgr_base_off%8==0);
    assert(rmgr_base_off%8==0);
    assert(imgr_base_off%8==0);

    wctx->pmgr = (struct pagechunk_mgr*)((uint8_t*)wctx + pmgr_base_off);
    wctx->rmgr = (struct reclaim_mgr*)((uint8_t*)wctx + rmgr_base_off);
    wctx->imgr = (struct iomgr*)((uint8_t*)wctx + imgr_base_off);

    _worker_init_pmgr(wctx->pmgr,opts);
    _worker_init_rmgr(wctx->rmgr,opts);
    _worker_init_imgr(wctx->imgr,opts);
}

struct worker_context* 
worker_init(struct worker_init_opts* opts)
{
    uint64_t size = 0;
    uint64_t nb_max_reqs = opts->max_request_queue_size_per_worker;
    uint32_t nb_max_slab_reclaim = opts->nb_reclaim_shards*opts->shard[0].nb_slabs;

    uint64_t pmgr_base_off;
    uint64_t rmgr_base_off;
    uint64_t imgr_base_off;

    //worker context
    size += sizeof(struct worker_context);
    size += sizeof(struct kv_request)*nb_max_reqs;
    size += pool_header_size(nb_max_reqs);
    size += nb_max_reqs*sizeof(struct kv_request_internal); //one req produces one req_internal.

    //page chunk manager
    //One kv request may produce one chunk request and ctx request
    //One mig request may produce one chunk request and ctx request
    //One post del may produce one chunk request and ctx request
    pmgr_base_off = size;
    size += sizeof(struct pagechunk_mgr);
    size += pool_header_size(nb_max_reqs*5); //pmgr_req_pool.
    size += pool_header_size(nb_max_reqs*5); //pmgr_ctx_pool.
    size += nb_max_reqs*5*sizeof(struct chunk_miss_callback);
    size += nb_max_reqs*5*sizeof(struct chunk_load_store_ctx);

    //recliam manager
    //One kv put/delete req may produce one post del request
    //One mig req may produce one post del request.
    rmgr_base_off = size;
    size += sizeof(struct reclaim_mgr);
    size += pool_header_size(nb_max_reqs*3);        //del_pool;
    size += pool_header_size(nb_max_reqs);          //mig_pool;
    size += pool_header_size(nb_max_slab_reclaim);  //slab_reclaim_request_pool;
    size += nb_max_reqs*3*sizeof(struct pending_item_delete);
    size += nb_max_reqs*sizeof(struct pending_item_migrate);
    size += nb_max_slab_reclaim*sizeof(struct slab_migrate_request);

    //io manager
    //One pmgr ctx req may produce two cache io
    //One cache io may produce two page io.
    imgr_base_off = size;
    size += sizeof(struct iomgr);
    size += pool_header_size(nb_max_reqs*10);   //cache_pool; one req may produce 2 cache io.
    size += pool_header_size(nb_max_reqs*20);   //page_pool; one cio may produce 2 page io.
    size += nb_max_reqs*10*sizeof(struct cache_io);
    size += nb_max_reqs*20*sizeof(struct page_io);

    struct worker_context *wctx = malloc(size);
    assert(wctx!=NULL);

    _worker_context_init(wctx,opts,pmgr_base_off,rmgr_base_off,imgr_base_off);
    return wctx;
}

static void
_rebuild_complete(void*ctx, int kverrno){
    struct worker_context *wctx = ctx;
    if(kverrno){
        SPDK_ERRLOG("Fails in database recovering\n");
        assert(0);
    }
    SPDK_NOTICELOG("Rebuild completes\n");
    //All initialization jobs complete.
    //Register pollers to start the service.
    struct spdk_poller *poller;
    poller = SPDK_POLLER_REGISTER(_worker_slab_evaluation_poll,wctx,DEFAULT_RECLAIM_POLLING_PERIOD_US);
    assert(poller!=NULL);

    poller = SPDK_POLLER_REGISTER(_worker_reclaim_poll,wctx,0);
    assert(poller!=NULL);

    poller = SPDK_POLLER_REGISTER(_worker_business_processor_poll,wctx,0);
    assert(poller!=NULL);
}

static void
_do_worker_start(void*ctx){
    struct worker_context* wctx = ctx;

    //I should get a io channel for the io manager.
    wctx->imgr->channel = spdk_bs_alloc_io_channel(wctx->target);
    assert( wctx->imgr->channel!=NULL);

    //Perform recovering for all slab file.
    //All kv request all blocked, before the recovering completes,
    SPDK_NOTICELOG("Start rebuilding\n");
    worker_perform_rebuild_async(wctx,_rebuild_complete,wctx);
}

void worker_start(struct worker_context* wctx){
    spdk_thread_send_msg(wctx->thread,_do_worker_start,wctx);
}

/************************************************************************************/
//Statistics related.

void worker_get_statistics(struct worker_context* wctx, struct worker_statistics* ws_out){
    ws_out->chunk_miss_times = wctx->pmgr->miss_times;
    ws_out->chunk_hit_times  = wctx->pmgr->hit_times;
    ws_out->nb_pending_reqs  = atomic_load(&wctx->sent_requests) - atomic_load(&wctx->processed_requests);
    ws_out->nb_pending_ios   = wctx->imgr->nb_pending_io;
    ws_out->nb_used_chunks   = wctx->pmgr->nb_used_chunks;
}
