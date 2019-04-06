/*
 * Copyright (C) Sogou, Inc
 */

#include <ngx_config.h> 
#include <ngx_core.h>
#include <ngx_http.h>


static ngx_int_t ngx_http_req_status_init_zone(ngx_shm_zone_t *shm_zone,
        void *data);
static void ngx_http_req_status_rbtree_insert_value(ngx_rbtree_node_t *temp,
        ngx_rbtree_node_t *node, ngx_rbtree_node_t *sentinel);
static ngx_int_t ngx_http_req_status_init(ngx_conf_t *cf);
static ngx_int_t ngx_http_req_status_handler(ngx_http_request_t *r);
static void *ngx_http_req_status_lookup(void *conf, ngx_uint_t hash,
        ngx_str_t *key);
static void ngx_http_req_status_expire(void *conf);

static void *ngx_http_req_status_create_main_conf(ngx_conf_t *cf);
static char *ngx_http_req_status_init_main_conf(ngx_conf_t *cf, void *conf);
static void *ngx_http_req_status_create_loc_conf(ngx_conf_t *cf);
static char *ngx_http_req_status_merge_loc_conf(ngx_conf_t *cf, void *parent,
        void *child);

static char *ngx_http_req_status_zone(ngx_conf_t *cf, ngx_command_t *cmd,
        void *conf);
static char *ngx_http_req_status(ngx_conf_t *cf, ngx_command_t *cmd, void *conf);
static char *ngx_http_req_status_show(ngx_conf_t *cf, ngx_command_t *cmd,
        void *conf);

static ngx_int_t ngx_http_req_status_show_handler(ngx_http_request_t *r);
//static ngx_int_t ngx_http_req_status_write_filter(ngx_http_request_t *r,
//        off_t bsent);

typedef struct ngx_http_req_status_loc_conf_s ngx_http_req_status_loc_conf_t;

typedef struct {
    ngx_uint_t                      requests;
    ngx_uint_t                      traffic;

    ngx_uint_t                      qps; 		//added by zk
    ngx_uint_t                      max_qps;

    ngx_uint_t                      bandwidth;
    ngx_uint_t                      max_bandwidth;

    ngx_uint_t                      max_active;
} ngx_http_req_status_data_t;

typedef struct {
    ngx_rbtree_node_t               node;   //这里的第一个字段居然是红黑树的节点 所以节点与此结构体的转换直接转换指针类型就可以
    ngx_queue_t                     queue;  //队列存在的意义是便于在展示的时候遍历

    ngx_uint_t                      count; // ref count

    ngx_http_req_status_data_t      data;

    ngx_uint_t                      active;
    ngx_uint_t                      last_traffic;          //为了带宽而引入的增量
    ngx_uint_t                      last_requests;          //added by zk 为了计算qps而引入的变量
    ngx_msec_t                      last_traffic_start;    //为了计算速度而引入的上次记录时间
    ngx_msec_t                      last_traffic_update;   //只是纯粹为了计算上次的更新时间

    ngx_uint_t                      len;
    u_char                          key[0];
} ngx_http_req_status_node_t;

typedef struct {
    ngx_rbtree_t                    rbtree;
    ngx_rbtree_node_t               sentinel;    //他就是红黑树的哨兵节点 主要的目的是让红黑树的叶子节点指向这个位置 方便之后的查找
    ngx_queue_t                     queue;
    time_t                          expire_lock;
} ngx_http_req_status_sh_t;

typedef struct {
    ngx_str_t                       *zone_name;
    ngx_http_req_status_node_t      *node;    //node内部不就含有data吗
    ngx_http_req_status_data_t      *pdata;
    ngx_http_req_status_data_t      data[0];
} ngx_http_req_status_print_item_t;

typedef struct {
    ngx_http_req_status_sh_t        *sh;
    ngx_slab_pool_t                 *shpool;
    ngx_shm_zone_t                  *shm_zone;
    ngx_http_complex_value_t        key;
} ngx_http_req_status_zone_t;

typedef struct {
    ngx_http_req_status_zone_t      *zone;      //主要监听的zone
    ngx_http_req_status_node_t      *node;	//这个是具体监听的内容 也就是最终要打印到屏幕的内容
} ngx_http_req_status_zone_node_t;

typedef struct {
    ngx_array_t                     req_zones;   //这个数组元素的类型就是上边的结构体
} ngx_http_req_status_ctx_t;

typedef struct {
    ngx_array_t                     zones;

    ngx_msec_t                      interval;   //这个是对于速度类统计的累计时间段
    time_t                          lock_time;  //每次展示之后 每个zone都需要锁定一段时间 锁定时不允许删除其中的节点
} ngx_http_req_status_main_conf_t;

struct ngx_http_req_status_loc_conf_s {
    ngx_array_t                     req_zones;
    ngx_http_req_status_loc_conf_t *parent;
};


static ngx_http_module_t ngx_http_req_status_module_ctx = {
    NULL,                                 /* preconfiguration */
    ngx_http_req_status_init,             /* postconfiguration */

    ngx_http_req_status_create_main_conf, /* create main configuration */
    ngx_http_req_status_init_main_conf,   /* init main configuration */

    NULL,                                 /* create server configuration */
    NULL,                                 /* merge server configuration */

    ngx_http_req_status_create_loc_conf,  /* create location configration */
    ngx_http_req_status_merge_loc_conf    /* merge location configration */
};


static ngx_command_t ngx_http_req_status_commands[] = {
    { ngx_string("req_status_interval"),
      NGX_HTTP_MAIN_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_msec_slot,
      0,
      offsetof(ngx_http_req_status_main_conf_t, interval), //为什么前两个要有这个offset呢   因为ngx_conf_set_msec_slot属于默认的解析函数 这里需要知道解析出来的内容放到了哪个位置上
      NULL },

    { ngx_string("req_status_lock_time"),
      NGX_HTTP_MAIN_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_sec_slot,
      0,
      offsetof(ngx_http_req_status_main_conf_t, lock_time),
      NULL },

    { ngx_string("req_status_zone"),
      NGX_HTTP_MAIN_CONF|NGX_CONF_TAKE3,
      ngx_http_req_status_zone,
      0,
      0,
      NULL
    },

    { ngx_string("req_status"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_1MORE,    //这里的块方式只是代表配置能否配置与此
      ngx_http_req_status,
      NGX_HTTP_LOC_CONF_OFFSET,    //这个属于loc块的配置 这个主要影响到上边的回调函数的传入参数 在这里参数是本模块的rlcf 上边的0是默认的cmcf
      0,
      NULL
    },

    { ngx_string("req_status_show"),
      NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_FLAG,
      ngx_http_req_status_show,
      0,                              //注意这里竟然将内容设置成了main 所以在上边的函数在调用时的参数是rmcf 这里并没有用到本模的rlcf
      0,
      NULL
    },

    //{ ngx_string("req_status_search"),   //added by zk
    //  NGX_HTTP_LOC_CONF|NGX_CONF_FLAG,
    //  ngx_http_req_status_show,
    //  0,
    //  0,
    //  NULL
    //},

    ngx_null_command
};

ngx_module_t ngx_http_req_status_module = {
    NGX_MODULE_V1,
    &ngx_http_req_status_module_ctx,
    ngx_http_req_status_commands,
    NGX_HTTP_MODULE,                          /* module type */
    NULL,                                     /* init master */
    NULL,                                     /* init module */
    NULL,                                     /* init process */
    NULL,                                     /* init thread */
    NULL,                                     /* exit thread */
    NULL,                                     /* exit process */
    NULL,                                     /* exit master */
    NGX_MODULE_V1_PADDING
};

//
//static ngx_int_t
//ngx_http_req_status_write_filter(ngx_http_request_t *r, off_t bsent)
//{
//    off_t                                   bytes;
//    ngx_uint_t                              i;
//    ngx_msec_t                              td;
//    ngx_http_req_status_ctx_t              *r_ctx;
//    ngx_http_req_status_data_t             *data;
//    ngx_http_req_status_zone_node_t        *pzn;
//    ngx_http_req_status_main_conf_t        *rmcf;
//
//    r_ctx = ngx_http_get_module_ctx(r, ngx_http_req_status_module);
//    if (r_ctx == NULL || r_ctx->req_zones.nelts == 0){
//        return NGX_DECLINED;
//    }
//
//    rmcf = ngx_http_get_module_main_conf(r, ngx_http_req_status_module);
//
//    pzn = r_ctx->req_zones.elts;
//
//    bytes = r->connection->sent - bsent;    //sent字段貌似是已经发送的字节数  bsent是上次发送的字节数
//
//    for (i = 0; i < r_ctx->req_zones.nelts; i++){    //这里边的数组是当下请求相关变量 以及与之相关的节点  这里的节点就是具体的数据了 每个请求最多只对应一个节点
//        data = &pzn[i].node->data;
//
//        ngx_shmtx_lock(&pzn[i].zone->shpool->mutex);    //这是针对变量的整个shpool池加锁的
//
//        data->traffic += bytes;   //更新traffic
//
//        if (ngx_current_msec > pzn[i].node->last_traffic_start){
//
//            td = ngx_current_msec - pzn[i].node->last_traffic_start;
//
//            if (td >= rmcf->interval){
//                data->bandwidth = pzn[i].node->last_traffic * 1000 / td;   //这个last_traffic是用来计算bandwidth的
//                if (data->bandwidth > data->max_bandwidth){
//                    data->max_bandwidth = data->bandwidth;                   //这个是bandwith的历史最大值
//                }
//
//                pzn[i].node->last_traffic = 0;    //清空last_traffic
//                pzn[i].node->last_traffic_start = ngx_current_msec;   //更新上次更新时间
//            }
//        }
//
//        pzn[i].node->last_traffic += bytes;   //更新last_traffic
//
//        if (ngx_current_msec > pzn[i].node->last_traffic_update){
//            pzn[i].node->last_traffic_update = ngx_current_msec;     //更新上次更新时间
//        }
//
//        ngx_shmtx_unlock(&pzn[i].zone->shpool->mutex);
//    }
//
//    return NGX_DECLINED;
//}

static void
ngx_http_req_status_rbtree_insert_value(ngx_rbtree_node_t *temp,
        ngx_rbtree_node_t *node, ngx_rbtree_node_t *sentinel)
{
    ngx_rbtree_node_t               **p;
    ngx_http_req_status_node_t      *cn, *cnt;

    for ( ;; ) {

        if (node->key < temp->key) {

            p = &temp->left;

        } else if (node->key > temp->key) {

            p = &temp->right;

        } else { /* node->key == temp->key */    //这一点还是挺重要的 如果两个不同的key 他们的hash值确实相同的  在这种情况下就需要比较存粹的key的大小  然后将他们放入到红黑树相应的位置

            cn = (ngx_http_req_status_node_t *) node;
            cnt = (ngx_http_req_status_node_t *) temp;

            p = (ngx_memn2cmp(cn->key, cnt->key, cn->len, cnt->len) < 0)
                ? &temp->left : &temp->right;
        }

        if (*p == sentinel) {     //这里的哨兵只是叶子空节点吧
            break;
        }

        temp = *p;
    }

    *p = node;
    node->parent = temp;
    node->left = sentinel;      //添加新节点  然后将新节点的两个叶子节点置为哨兵
    node->right = sentinel;
    ngx_rbt_red(node);          //添加完新节点 将此节点置位红色
}

static void
ngx_http_req_status_cleanup(void *data)
{
    ngx_uint_t                          i;
    ngx_http_req_status_ctx_t          *r_ctx = data;
    ngx_http_req_status_zone_node_t    *pzn;

    if (r_ctx->req_zones.nelts == 0)
        return;

    pzn = r_ctx->req_zones.elts;

    for (i = 0; i < r_ctx->req_zones.nelts; i++){
        ngx_shmtx_lock(&pzn[i].zone->shpool->mutex);

        pzn[i].node->count --;  //引用计数减少
        pzn[i].node->active --;

        ngx_shmtx_unlock(&pzn[i].zone->shpool->mutex);
    }
}

static ngx_int_t
ngx_http_req_status_handler(ngx_http_request_t *r)
{
    size_t                              len;
    uint32_t                            hash;
    ngx_str_t                           key;
    ngx_uint_t                          i;
    ngx_shm_zone_t                    **pzone;
    ngx_pool_cleanup_t                 *cln;
    ngx_http_req_status_ctx_t          *r_ctx;
    ngx_http_req_status_zone_t         *ctx;
    ngx_http_req_status_node_t         *ssn;
    ngx_http_req_status_loc_conf_t     *rlcf;
    ngx_http_req_status_zone_node_t    *pzn;

    if (r->headers_in.content_length_n <= 0) return NGX_DECLINED;  //added by zk   这里可能导致一些空体请求被漏掉

    r_ctx = ngx_http_get_module_ctx(r, ngx_http_req_status_module);

    rlcf = ngx_http_get_module_loc_conf(r, ngx_http_req_status_module);

    do {
        pzone = rlcf->req_zones.elts;

        for (i = 0; i < rlcf->req_zones.nelts; i++) {    //遍历当下locatiton需要监听的key   注意当下location当下没有设置req_status 则数组大小为0
            ctx = pzone[i]->data;

            if (ngx_http_complex_value(r, &ctx->key, &key) != NGX_OK) {    //这里是将之前解析到的复合变量 作用域当前的请求 得到当前请求在这个复合变量上的字符串（如：10.16.15.56:0nomiss）
                continue;
            }

            if (key.len == 0) {       //也有可能解析不出来吗
                continue;
            }

            if (key.len > 65535) {
                ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                        "req-status, the value of the \"%V\" variable "
                        "is more than 65535 bytes: \"%v\"",
                        &ctx->key.value, &key);
                continue;
            }

            if (r_ctx == NULL) {    //请求第一次来 所以上下文为空  这个跟ctx完全是两个东西

                r_ctx = ngx_palloc(r->pool, sizeof(ngx_http_req_status_ctx_t));
                if (r_ctx == NULL) {
                    return NGX_HTTP_INTERNAL_SERVER_ERROR;
                }

                if (ngx_array_init(&r_ctx->req_zones, r->pool, 2,   //这里初始化两个元素
                            sizeof(ngx_http_req_status_zone_node_t))
                        != NGX_OK)
                {
                    return NGX_HTTP_INTERNAL_SERVER_ERROR;
                }

                cln = ngx_pool_cleanup_add(r->pool, 0);   //在r->pool中生成一个cln  便于最终的回收
                if (cln == NULL) {
                    return NGX_HTTP_INTERNAL_SERVER_ERROR;
                }

                cln->handler = ngx_http_req_status_cleanup;
                cln->data = r_ctx;

                ngx_http_set_ctx(r, r_ctx, ngx_http_req_status_module);   //设置request的上下文
            }

            hash = ngx_crc32_short(key.data, key.len);

            ngx_shmtx_lock(&ctx->shpool->mutex);

            ssn = ngx_http_req_status_lookup(ctx, hash, &key);

            if (ssn == NULL) {                           //本过滤请求第一次来到  需要建立这个请求的红黑树节点
                len  = sizeof(ngx_http_req_status_node_t) + key.len + 1;   //这里除了本身结构体的大小  还包括存在这个结构体之后的key字符串的大小

                ssn = ngx_slab_alloc_locked(ctx->shpool, len);       //这里对其分配的内存是存在shpool池中
                if (ssn == NULL) {
                    ngx_http_req_status_expire(ctx);                 //这个函数只有这个位置用到了 就是在第一次在共享内存中分配节点时失败 会进行第二次的分配 在此之前会执行这个函数

                    ssn = ngx_slab_alloc_locked(ctx->shpool, len);
                    if (ssn == NULL) {
                        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                                "req-status, slab alloc fail, zone = \"%V\", "
                                "key = \"%V\", size = %uz",
                                &ctx->shm_zone->shm.name, &key, len);

                        ngx_shmtx_unlock(&ctx->shpool->mutex);

                        continue;
                    }
                }

                ssn->node.key = hash;                //这里是红黑树的节点  将hash作为key的值
                ssn->len = key.len;
                ssn->count = 1;

                ngx_memzero(&ssn->data, sizeof(ssn->data));   //更具体的监听内容  这里给清空了
                ngx_memcpy(ssn->key, key.data, key.len);     //这里的key是这个请求分离出来的串（nomiss:0.0.0.0）
                ssn->key[key.len] = '\0';
                ssn->last_traffic_update = 0;

                ngx_rbtree_insert(&ctx->sh->rbtree, &ssn->node);    //将新的节点放进到红黑树中
            }

            ssn->data.requests ++;      //这个requests是递增的                    //这里会随着请求的过来 将必须要更新的字段更新  进流量的监控是都可以在此刻更新的 正如下面我添加的代码    而出流量的监控是需要在包体发送的时候进行统计  这时可能是多次进入本请求的发送的总和
            ssn->active ++;              //这个active会随着本request的消亡而递减
            if (ssn->active > ssn->data.max_active) {
                ssn->data.max_active = ssn->active;   //这里的最大值是
            }


// added by zk
	ngx_http_req_status_data_t *data = &ssn->data;
	//if (r->headers_in.content_length_n > 0)
	data->traffic += r->headers_in.content_length_n;   //更新traffic

        if (ngx_current_msec > ssn->last_traffic_start){

            ngx_msec_t td = ngx_current_msec - ssn->last_traffic_start;

	    ngx_http_req_status_main_conf_t *rmcf = ngx_http_get_module_main_conf(r, ngx_http_req_status_module);
            if (td >= rmcf->interval){
                data->bandwidth = ssn->last_traffic * 1000 / td;   //这个last_traffic是用来计算bandwidth的
		data->qps = ssn->last_requests * 1000 / td;
                if (data->bandwidth > data->max_bandwidth){
                    data->max_bandwidth = data->bandwidth;                   //这个是bandwith的历史最大值
                }
                if (data->qps > data->max_qps){
                    data->max_qps = data->qps;                   //这个是bandwith的历史最大值
                }

                ssn->last_traffic = 0;    			//清空last_traffic
		ssn->last_requests = 0;
                ssn->last_traffic_start = ngx_current_msec;     //更新上次更新时间
            }
        }

        ssn->last_traffic += r->headers_in.content_length_n;   //更新last_traffic
        //ngx_log_error(NGX_LOG_ALERT, ngx_cycle->log, 0, "length1 = %d  length2 = %d", r->request_length, r->headers_in.content_length_n);
        ssn->last_requests ++;   //更新last_traffic

        if (ngx_current_msec > ssn->last_traffic_update){
            ssn->last_traffic_update = ngx_current_msec;     //更新上次更新时间
        }
// --------------------

            ngx_queue_insert_head(&ctx->sh->queue, &ssn->queue);    //这里用到了sh的queue  之前如果找到了会从对应的位置删掉 这里有加了进去

            ngx_shmtx_unlock(&ctx->shpool->mutex);

            pzn = ngx_array_push(&r_ctx->req_zones);   //放入上下文数组
            if (pzn == NULL) {
                return NGX_HTTP_INTERNAL_SERVER_ERROR;
            }

            pzn->node = ssn;                   //将此红黑树节点  加入到本次请求的上下文中  便于之后在请求发送包体时进行监控计数  并且包体的发送可能分多次进入事件循环才能结束
            pzn->zone = ctx;
        }

        rlcf = rlcf->parent;    //这里需要递归记录   也就是说 可以将本location中请求需要记录的内容记录到本location中  也可以同时计入上层的location中   使得上层要计入的内容包括下层要计入的内容  而不是进行默认的配置merge
    } while (rlcf);

    return NGX_DECLINED;
}

static void *
ngx_http_req_status_lookup(void *conf, ngx_uint_t hash, ngx_str_t *key)
{
    ngx_int_t                       rc;
    ngx_rbtree_node_t              *node, *sentinel;
    ngx_http_req_status_node_t     *ssn;
    ngx_http_req_status_zone_t     *ctx = conf;

    node = ctx->sh->rbtree.root;
    sentinel = ctx->sh->rbtree.sentinel;   //这里可以看出sentinel是树的叶子节点的左右指针指向的位置

    while (node != sentinel) {
        if (hash < node->key) {
            node = node->left;
            continue;
        }

        if (hash > node->key) {
            node = node->right;
            continue;
        }

        /* hash == node->key */
        ssn = (ngx_http_req_status_node_t *)node;   //可以直接赋值是因为 红黑树节点存在于statusnode结构中

        rc = ngx_memn2cmp(key->data, ssn->key, key->len, ssn->len);
        if (rc == 0){                         //这个key找到了
            ngx_queue_remove(&ssn->queue);    //这里先从队列之中移除 之后在加入  这个我才有点类似于先入先出队列 

            ssn->count ++;   //增加引用计数

            return ssn;
        }

        node = (rc < 0) ? node->left : node->right;   //key还是没有找到 但是找到key的hash相同的那个节点了  接续在这个子树内进行搜索
    }

    return NULL;
}

static void
ngx_http_req_status_expire(void *conf)    //这个函数的目的其实是为了删除一个节点 缓解满了的情况 但是在删除之前需要判断两件事
					//1 离上次展示后要有足够长的时间
					//2 离上次更新traffic要有足够长的时间
					//为什么要这样 我还不清楚
{
    ngx_queue_t                     *q;
    ngx_http_req_status_zone_t      *ctx = conf;
    ngx_http_req_status_node_t      *ssn;

    if (ngx_queue_empty(&ctx->sh->queue)) {
        return;
    }

    if (ctx->sh->expire_lock > ngx_time()){   //意思似乎是没过期的话
        return;
    }

    q =  ngx_queue_last(&ctx->sh->queue);

    ssn = ngx_queue_data(q, ngx_http_req_status_node_t, queue);    //满了 就把队列中最后一个节点删除  当然之前需要判断是否过期  之后还需要判断  traffic更新的时间  不能间隔太短

    if (!ssn->data.requests || (ngx_current_msec > ssn->last_traffic_update && 
                ngx_current_msec - ssn->last_traffic_update >= 10 * 1000)){
        ngx_log_debug2(NGX_LOG_DEBUG_HTTP, ngx_cycle->log, 0,
                "req-status, release node, zone = \"%V\", key = \"%s\"",
                &ctx->shm_zone->shm.name, ssn->key);

        ngx_queue_remove(q);

        ngx_rbtree_delete(&ctx->sh->rbtree, &ssn->node);

        ngx_slab_free_locked(ctx->shpool, ssn);
    }
}


static u_char *
ngx_http_req_status_format_size(u_char *buf, ngx_uint_t v)
{
    u_char              scale;
    ngx_uint_t          size;

    if (v > 1024 * 1024 * 1024 - 1) {
        size = v / (1024 * 1024 * 1024);
        if ((v % (1024 * 1024 * 1024)) > (1024 * 1024 * 1024 / 2 - 1)) {
            size++;
        }
        scale = 'G';
    } else if (v > 1024 * 1024 - 1) {
        size = v / (1024 * 1024);
        if ((v % (1024 * 1024)) > (1024 * 1024 / 2 - 1)) {
            size++;
        }
        scale = 'M';
    } else if (v > 9999) {
        size = v / 1024;
        if (v % 1024 > 511) {
            size++;
        }
        scale = 'K';
    } else {
        size = v;
        scale = '\0';
    }

    if (scale) {
        return ngx_sprintf(buf, "%ui%c", size, scale);
    }

    return ngx_sprintf(buf, " %ui", size);
}

static int ngx_libc_cdecl
ngx_http_req_status_cmp_items(const void *one, const void *two)
{
    ngx_http_req_status_print_item_t *first = (ngx_http_req_status_print_item_t*) one;
    ngx_http_req_status_print_item_t *second = (ngx_http_req_status_print_item_t*) two;

    return (int)(first->zone_name == second->zone_name) ? 
        ngx_strcmp(first->node->key, second->node->key) :
        ngx_strncmp(first->zone_name->data, second->zone_name->data, first->zone_name->len);
}

static ngx_int_t
ngx_http_req_status_show_handler(ngx_http_request_t *r)
{
    size_t                              size, item_size;
    u_char                              long_num, full_info, clear_status;
    ngx_int_t                           rc;
    ngx_buf_t                          *b;
    ngx_uint_t                          i;
    ngx_array_t                         items;
    ngx_queue_t                        *q;
    ngx_chain_t                         out;
    ngx_http_req_status_zone_t        **pzone;
    ngx_http_req_status_node_t         *rsn;
    ngx_http_req_status_main_conf_t    *rmcf;
    ngx_http_req_status_print_item_t   *item;
    static u_char                       header[] = 
        "zone_name\tkey\tmax_active\tmax_bw\ttraffic\trequests\t"
        "active\tbandwidth\tqps\tmax_qps\n";

    if (r->method != NGX_HTTP_GET && r->method != NGX_HTTP_HEAD) {
        return NGX_HTTP_NOT_ALLOWED;
    }

    rc = ngx_http_discard_request_body(r);

    if (rc != NGX_OK) {
        return rc;
    }

    ngx_str_set(&r->headers_out.content_type, "text/plain");

    if (r->method == NGX_HTTP_HEAD) {
        r->headers_out.status = NGX_HTTP_OK;

        rc = ngx_http_send_header(r);

        if (rc == NGX_ERROR || rc > NGX_OK || r->header_only) {
            return rc;
        }
    }

#define NGX_STRCHR(s, u)  \
    (ngx_strlchr((s)->data, (s)->data + (s)->len, u) != NULL)

    full_info = NGX_STRCHR(&r->args, 'f');
    long_num = NGX_STRCHR(&r->args, 'l');
    clear_status = NGX_STRCHR(&r->args, 'c');

    item_size = sizeof(ngx_http_req_status_print_item_t) +
        (clear_status ? sizeof(ngx_http_req_status_data_t) : 0);

    if (ngx_array_init(&items, r->pool, 40, item_size) != NGX_OK){
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    size = sizeof(header) - 1;

    rmcf = ngx_http_get_module_main_conf(r, ngx_http_req_status_module);

    pzone = rmcf->zones.elts;

    for (i = 0; i < rmcf->zones.nelts; i++){
        ngx_shmtx_lock(&pzone[i]->shpool->mutex);

        for (q = ngx_queue_last(&pzone[i]->sh->queue);
                q != ngx_queue_sentinel(&pzone[i]->sh->queue);
                q = ngx_queue_prev(q))
        {
            rsn = ngx_queue_data(q, ngx_http_req_status_node_t, queue);
            if (!rsn->data.requests){
                continue;
            }

            if (rsn->last_traffic){    //这个适用于生成带宽的临时大小  
                if (ngx_current_msec > rsn->last_traffic_update &&
                        ngx_current_msec - rsn->last_traffic_update >= 
                        rmcf->interval){                          //如果上次更新的时间太久 带宽相关的内容一定没有意义了  所以将其清空
                    rsn->last_traffic_start = 0;
                    rsn->last_traffic = 0;
                    rsn->last_requests = 0;     //added by zk
                    rsn->data.bandwidth = 0;
                    rsn->data.qps = 0;          //added by zk 
                    rsn->last_traffic_update = ngx_current_msec;
                }
            }

            size += pzone[i]->shm_zone->shm.name.len +    //这个是自己定义的key的大小
                rsn->len + (sizeof("\t") - 1) * 10 +        //第一个是具体的key在本请求中得到的字符串  modified by zk 8 -> 10   
                (NGX_INT64_LEN) * 8;    //这个算的是int64所占用的字符串   modified by zk  6 -> 8

            if (full_info){
                size += (NGX_INT64_LEN) * 3 + (sizeof("\t") - 1) * 3;    //这里统计的字符串都要-1  为了去掉结尾的0终结父
            }

            item = ngx_array_push(&items);
            if (item == NULL){
                return NGX_HTTP_INTERNAL_SERVER_ERROR;
            }

            item->zone_name = &pzone[i]->shm_zone->shm.name;    //将要统计的信息加入到items  这里的name就是自定义的key
            item->node = rsn;

            if (clear_status){     //这里clear的时候会将所有的存储信息清空 但需要返回当下的统计信息  所以在清空的同时还需要同时保存当下信息  以便于将这次的结果返回
                item->pdata = NULL;
                ngx_memcpy(&item->data[0], &rsn->data, sizeof(ngx_http_req_status_data_t));    //将节点的data内容拷贝到item后边
                ngx_memzero(&rsn->data, sizeof(ngx_http_req_status_data_t));                   //然后将节点的data内存请0
            } else {
                item->pdata = &rsn->data;      //将pdata执行具体节点的数据内容
            }
        }

        pzone[i]->sh->expire_lock = ngx_time() + rmcf->lock_time;   //关于expirelock和locktime只在这里用到 似乎是为了在展示之后的一段时间 不能删掉他里边的节点  另外ngx_time似乎是返回当下的时间  这个时间与ngx_current_msec有啥区别呢

        ngx_shmtx_unlock(&pzone[i]->shpool->mutex);
    }

    if (items.nelts > 1) {
        ngx_qsort(items.elts, (size_t) items.nelts, item_size,    //还需要将内容进行排序
                ngx_http_req_status_cmp_items);
    }

    b = ngx_create_temp_buf(r->pool, size);     //分配足够大的内存
    if (b == NULL) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    b->last = ngx_cpymem(b->last, header, sizeof(header) - 1);   //将头传入缓冲区

    item = items.elts;

    for (i = 0; i < items.nelts; i++){
        if (i) {
            item = (ngx_http_req_status_print_item_t *)
                ((u_char *)item + item_size);
        }

        /* set pdata here because of qsort above */
        if (item->pdata == NULL){
            item->pdata = &item->data[0];   //这里应该特指clear的时候 
        }

        b->last = ngx_cpymem(b->last, item->zone_name->data, item->zone_name->len);    //已经定义好的key
        *b->last ++ = '\t';

        b->last = ngx_cpymem(b->last, item->node->key, item->node->len);	       //根据请求和key 解析出来的字符串
        *b->last ++ = '\t';

        if (long_num){
            b->last = ngx_sprintf(b->last, "%ui\t%ui\t%ui\t%ui\t%ui\t%ui\t%ui\t%ui",
                    item->pdata->max_active,
                    item->pdata->max_bandwidth,  //编程B
                    item->pdata->traffic,
                    item->pdata->requests,
                    item->node->active,
                    item->pdata->bandwidth,
                    item->pdata->qps,
                    item->pdata->max_qps);
        } else {
            b->last = ngx_sprintf(b->last, "%ui\t", item->pdata->max_active);
            b->last = ngx_http_req_status_format_size(b->last,      //对其进行或K或M或G的格式化
                    item->pdata->max_bandwidth);
            *b->last ++ = '\t';

            b->last = ngx_http_req_status_format_size(b->last,
                    item->pdata->traffic);
            *b->last ++ = '\t';

            b->last = ngx_sprintf(b->last, "%ui\t", item->pdata->requests);
            b->last = ngx_sprintf(b->last, "%ui\t", item->node->active);

            b->last = ngx_http_req_status_format_size(b->last,
                    item->pdata->bandwidth);
            *b->last ++ = '\t';
            b->last = ngx_http_req_status_format_size(b->last,
                    item->pdata->qps);
            *b->last ++ = '\t';
            b->last = ngx_http_req_status_format_size(b->last,
                    item->pdata->max_qps);
        }

        if (full_info){
            b->last = ngx_sprintf(b->last, "\t%ui\t%ui\t%ui",
                    item->node->last_traffic,
                    item->node->last_traffic_start,
                    item->node->last_traffic_update);
        }

        *b->last ++ = '\n';
    }

    out.buf = b;
    out.next = NULL;

    r->headers_out.status = NGX_HTTP_OK;
    r->headers_out.content_length_n = b->last - b->pos;

    b->last_buf = 1;

    rc = ngx_http_send_header(r);

    if (rc == NGX_ERROR || rc > NGX_OK || r->header_only) {
        return rc;
    }

    return ngx_http_output_filter(r, &out);
}

static void *
ngx_http_req_status_create_main_conf(ngx_conf_t *cf)
{
    ngx_http_req_status_main_conf_t   *rmcf;

    rmcf = ngx_pcalloc(cf->pool, sizeof(ngx_http_req_status_main_conf_t));
    if (rmcf == NULL) {
        return NULL;
    }

    if (ngx_array_init(&rmcf->zones, cf->pool, 4,
                sizeof(ngx_http_req_status_zone_t *)) != NGX_OK)
    {
        return NULL;
    }

    rmcf->interval = NGX_CONF_UNSET_MSEC;
    rmcf->lock_time = NGX_CONF_UNSET;

    return rmcf;
}

static char *
ngx_http_req_status_init_main_conf(ngx_conf_t *cf, void *conf)
{
    ngx_http_req_status_main_conf_t *rmcf = conf;

    ngx_conf_init_msec_value(rmcf->interval, 3000);
    ngx_conf_init_value(rmcf->lock_time, 10);

    return NGX_CONF_OK;
}

static void *
ngx_http_req_status_create_loc_conf(ngx_conf_t *cf)
{
    ngx_http_req_status_loc_conf_t *rlcf;

    rlcf = ngx_pcalloc(cf->pool, sizeof(ngx_http_req_status_loc_conf_t));    //在这里就分配了空间 其中包括了所有可能的位置 main srv loc   但是具体根据配置赋值是在解析配置的时候
    if (rlcf == NULL) {
        return NULL;
    }

    rlcf->parent = NGX_CONF_UNSET_PTR;

    return rlcf;
}

static char *
ngx_http_req_status_merge_loc_conf(ngx_conf_t *cf, void *parent, void *child)
{
    ngx_http_req_status_loc_conf_t *prev = parent;
    ngx_http_req_status_loc_conf_t *conf = child;
    ngx_http_req_status_loc_conf_t *rlcf;

    if (conf->parent == NGX_CONF_UNSET_PTR){   //如果当前的parent还没有赋值
        rlcf = prev;

        if (rlcf->parent == NGX_CONF_UNSET_PTR) {
            rlcf->parent = NULL;
        } else {
            while (rlcf->parent && rlcf->req_zones.nelts == 0) {    
                rlcf = rlcf->parent;
            }
        }

        conf->parent = rlcf->req_zones.nelts ? rlcf : NULL;   //其实就是这样反向构造一个数
    }

    return NGX_CONF_OK;
}

static ngx_int_t
ngx_http_req_status_init_zone(ngx_shm_zone_t *shm_zone, void *data)    //第二个参数用在reload的时候  原始的data与新的data的切换 大概是为了reload的时候保障监控数据不丢失
{
    size_t                      len;
    ngx_http_req_status_zone_t *ctx = shm_zone->data;
    ngx_http_req_status_zone_t *octx = data;

    if (octx){
        if (ngx_strcmp(&octx->key.value, &ctx->key.value) != 0) {    //新老不相等 
            ngx_log_error(NGX_LOG_EMERG, shm_zone->shm.log, 0,
                    "req_status \"%V\" uses the \"%V\" variable "
                    "while previously it used the \"%V\" variable",
                    &shm_zone->shm.name, &ctx->key.value, &octx->key.value);
            return NGX_ERROR;
        }

        ctx->sh = octx->sh;
        ctx->shpool = octx->shpool;

        return NGX_OK;
    }

    ctx->shpool = (ngx_slab_pool_t *)shm_zone->shm.addr;

    if (shm_zone->shm.exists){                      //这个存在还不太确定是什么意思
        ctx->sh = ctx->shpool->data;

        return NGX_OK;
    }

    ctx->sh = ngx_slab_alloc(ctx->shpool, sizeof(ngx_http_req_status_sh_t));   //从这个slab上边分配一个sh结构
    if (ctx->sh == NULL){
        return NGX_ERROR;
    }

    ctx->shpool->data = ctx->sh;     //看来data是代表sh首位置

    ngx_rbtree_init(&ctx->sh->rbtree, &ctx->sh->sentinel,    //对内部结构的初始化 这里sentinel就是根节点 也是初始的哨兵
            ngx_http_req_status_rbtree_insert_value);        //这里是插入回调   具体插入节点的分配内存在其他位置  当然这个分配也必须是在shpool池中

    ngx_queue_init(&ctx->sh->queue);

    ctx->sh->expire_lock = 0;    //这个初始化为0 在展示的时候赋值  在需要过期删除的时候用到

    len = sizeof("in req_status zone \"\"") + shm_zone->shm.name.len;

    ctx->shpool->log_ctx = ngx_slab_alloc(ctx->shpool, len);
    if (ctx->shpool->log_ctx == NULL) {
        return NGX_ERROR;
    }

    ngx_sprintf(ctx->shpool->log_ctx, " in req_status zone \"%V\"%Z",
            &shm_zone->shm.name);

    return NGX_OK;
}

static char *
ngx_http_req_status_zone(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ssize_t                             size;
    ngx_str_t                          *value;
    ngx_http_req_status_zone_t         *ctx, **pctx;
    ngx_http_req_status_main_conf_t    *rmcf;
    ngx_http_compile_complex_value_t    ccv;

    value = cf->args->elts;

    size = ngx_parse_size(&value[3]);    //就是那个256k

    if (size == NGX_ERROR) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                "invalid size of %V \"%V\"", &cmd->name, &value[3]);
        return NGX_CONF_ERROR;
    }

    if (size < (ssize_t) (8 * ngx_pagesize)) { 
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                "%V \"%V\" is too small", &cmd->name, &value[1]);
        return NGX_CONF_ERROR;
    }

    ctx = ngx_pcalloc(cf->pool, sizeof(ngx_http_req_status_zone_t));    //在cf的pool上
    if (ctx == NULL){
        return NGX_CONF_ERROR;
    }

    ngx_memzero(&ccv, sizeof(ngx_http_compile_complex_value_t));

    ccv.cf = cf;
    ccv.value = &value[2];                      //这里是定义的nginx内部变量名字
    ccv.complex_value = &ctx->key;      	//这个大概是解析后的内容要存放的位置

    if (ngx_http_compile_complex_value(&ccv) != NGX_OK) {   //这一步目的是  将nginx变量字符串  转换为   ctx->key 这时一个解析后的复杂变量类型
        return NGX_CONF_ERROR;
    }

    ctx->shm_zone = ngx_shared_memory_add(cf, &value[1], size,   //1 代表定定定定定定定定定义key的名字 这里实在具体的分配共享内存
            &ngx_http_req_status_module);
    if (ctx->shm_zone == NULL) {
        return NGX_CONF_ERROR;
    }

    if (ctx->shm_zone->data) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                "%V \"%V\" is already bound",
                &cmd->name, &value[1]);
        return NGX_CONF_ERROR;
    }

    ctx->shm_zone->init = ngx_http_req_status_init_zone;
    ctx->shm_zone->data = ctx;                               //注意这里的data不是共享内存 而是一个对外的暂存，这里存放对应的ctx

    rmcf = ngx_http_conf_get_module_main_conf(cf, ngx_http_req_status_module);  //注意  这里没有用到conf  是直接通过cf取到的

    pctx = ngx_array_push(&rmcf->zones); 

    if (pctx == NULL){
        return NGX_CONF_ERROR;
    }

    *pctx = ctx;   //将其保存到本模块的配置结构之中

    return NGX_CONF_OK;
}

static char *
ngx_http_req_status(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)   //cf为当前解析的配置项的值 cmd是解析的配置项的值  conf是模块的定义好的conf（可能是main、server、location）
{
    ngx_http_req_status_loc_conf_t *rlcf = conf;

    ngx_str_t                      *value;
    ngx_uint_t                      i, m;
    ngx_shm_zone_t                 *shm_zone, **zones, **pzone;

    value = cf->args->elts;

    zones = rlcf->req_zones.elts;

    for (i = 1; i < cf->args->nelts; i++){    //遍历本location中需要监听的key
        if (value[i].data[0] == '@') {   //这里还允许@   但也没看出有啥作用
            rlcf->parent = NULL;

            if (value[i].len == 1) {
                continue;
            }

            value[i].data ++;
            value[i].len --;
        }

        shm_zone = ngx_shared_memory_add(cf, &value[i], 0,    //这里应该是在找之前已经存储到的共享内存   这里的value就是之前定义的key名字  
                &ngx_http_req_status_module);
        if (shm_zone == NULL) { 
		return NGX_CONF_ERROR;
        }

        if (zones == NULL) {
            if (ngx_array_init(&rlcf->req_zones, cf->pool, 2, sizeof(ngx_shm_zone_t *))    //这里的pool用cf的
                    != NGX_OK)
            {
                return NGX_CONF_ERROR;
            }

            zones = rlcf->req_zones.elts;
        }

        for (m = 0; m < rlcf->req_zones.nelts; m++) {
            if (shm_zone == zones[m]) {				//在已有的共享内存中找指定的内容   这里是指针的内容
                return "is duplicate";
            }
        }

        pzone = ngx_array_push(&rlcf->req_zones);
        if (pzone == NULL){
            return NGX_CONF_ERROR;
        }

        *pzone = shm_zone;   //如果没有的话  将其加入
    } 

    return NGX_CONF_OK;
}

static char *
ngx_http_req_status_show(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    //if (ngx_strncmp(cmd->name.data, "req_status_show",15)) {   //added by zk
    //    search = false;
    //} else {
    //    search = true;
    //}

    ngx_http_core_loc_conf_t  *clcf;

    clcf = ngx_http_conf_get_module_loc_conf(cf, ngx_http_core_module);
    clcf->handler = ngx_http_req_status_show_handler;

    return NGX_CONF_OK;
}

static ngx_int_t
ngx_http_req_status_init(ngx_conf_t *cf)
{
    ngx_http_handler_pt               *h;
    ngx_http_core_main_conf_t         *cmcf;
    ngx_http_req_status_main_conf_t   *rmcf;

    rmcf = ngx_http_conf_get_module_main_conf(cf, ngx_http_req_status_module);

    if (rmcf->zones.nelts == 0){
        return NGX_OK;
    }

    cmcf = ngx_http_conf_get_module_main_conf(cf, ngx_http_core_module);

    h = ngx_array_push(&cmcf->phases[NGX_HTTP_PREACCESS_PHASE].handlers);
    if (h == NULL) {
        return NGX_ERROR;
    }

    *h = ngx_http_req_status_handler;

//    ngx_http_top_write_filter = ngx_http_req_status_write_filter;

    return NGX_OK;
}
