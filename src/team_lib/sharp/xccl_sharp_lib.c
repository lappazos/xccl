/*
 * Copyright (C) Mellanox Technologies Ltd. 2020.  ALL RIGHTS RESERVED.
 * See file LICENSE for terms.
 */
#include "xccl_sharp_lib.h"
#include "xccl_sharp_collective.h"
#include "xccl_sharp_map.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/time.h>
#include <unistd.h>
#include <inttypes.h>

static ucs_config_field_t xccl_team_lib_sharp_config_table[] = {
    {"", "", NULL,
        ucs_offsetof(xccl_team_lib_sharp_config_t, super),
        UCS_CONFIG_TYPE_TABLE(xccl_team_lib_config_table)
    },

    {NULL}
};

unsigned int xccl_sharp_global_rand_state;

static inline int xccl_sharp_rand()
{
    return rand_r(&xccl_sharp_global_rand_state);
}

static inline void xccl_sharp_rand_state_init(unsigned int *state)
{
    struct timeval tval;
    gettimeofday(&tval, NULL);
    *state = (unsigned int)(tval.tv_usec ^ getpid());
}

static inline void xccl_sharp_global_rand_state_init()
{
    xccl_sharp_rand_state_init(&xccl_sharp_global_rand_state);
}

static xccl_status_t xccl_sharp_lib_open(xccl_team_lib_h self,
                                         xccl_team_lib_config_t *config) {
    xccl_team_lib_sharp_t        *tl  = xccl_derived_of(self, xccl_team_lib_sharp_t);
    xccl_team_lib_sharp_config_t *cfg = xccl_derived_of(config, xccl_team_lib_sharp_config_t);
    
    tl->log_component.log_level = cfg->super.log_component.log_level;
    sprintf(tl->log_component.name, "%s", "TEAM_SHARP");
    xccl_sharp_debug("Team SPARP opened");
    if (cfg->super.priority != -1) {
        tl->super.priority = cfg->super.priority;
    }
    setenv("SHARP_COLL_NUM_COLL_GROUP_RESOURCE_ALLOC_THRESHOLD", "0", 0);
    xccl_sharp_global_rand_state_init();
    map_xccl_to_sharp_dtype();
    map_xccl_to_sharp_reduce_op_type();

    return XCCL_OK;
}

static int xccl_sharp_oob_barrier(void *context)
{
    xccl_oob_collectives_t *oob = (xccl_oob_collectives_t*)context;
    int comm_size = oob->size;
    char *tmp = NULL, c = 'c';
    tmp = (char*)malloc(comm_size * sizeof(char));
    xccl_oob_allgather(&c, tmp, sizeof(char), oob);
    free(tmp);
    return 0;
}

static int xccl_sharp_oob_gather(void *context, int root, void *sbuf,
                                 void *rbuf, int size)
{
    xccl_oob_collectives_t *oob = (xccl_oob_collectives_t*)context;
    int comm_size = oob->size;
    int comm_rank = oob->rank;
    void *tmp = NULL;

    if (comm_rank != root) {
        tmp = malloc(comm_size*size);
        rbuf = tmp;
    }
    xccl_oob_allgather(sbuf, rbuf, size, oob);
    if (tmp) {
        free(tmp);
    }
    return 0;
}

static int xccl_sharp_oob_bcast(void *context, void *buf, int size, int root)
{
    xccl_oob_collectives_t *oob = (xccl_oob_collectives_t*)context;
    int comm_size = oob->size;
    int comm_rank = oob->rank;
    void *tmp;
    tmp = malloc(comm_size*size);
    xccl_oob_allgather(buf, tmp, size, oob);
    if (comm_rank != root) {
        memcpy(buf, (void*)((ptrdiff_t)tmp + root*size), size);
    }
    free(tmp);
    return 0;
}

static ucs_status_t xccl_sharp_rcache_mem_reg_cb(void *context,
                                                 ucs_rcache_t *rcache,
                                                 void *arg,
                                                 ucs_rcache_region_t *rregion,
                                                 uint16_t rcache_mem_reg_flags)
{
    xccl_sharp_context_t       *ctx    = (xccl_sharp_context_t*) context;
    xccl_sharp_rcache_region_t *region = xccl_derived_of(rregion, xccl_sharp_rcache_region_t);
    void                       *addr   = (void*)region->super.super.start;
    size_t                     length  = region->super.super.end - region->super.super.start;
    int rc;

    rc = sharp_coll_reg_mr(ctx->sharp_context, addr, length, &region->memh);
    if (rc != SHARP_COLL_SUCCESS) {
        xccl_sharp_error("SHARP regmr failed\n");
        return UCS_ERR_NO_MESSAGE;
    }
    xccl_sharp_debug("RCACHE, mem_reg, addr %p, len %zd, memh %p\n",
                     addr, length, region->memh);
    return UCS_OK;
}

static void xccl_sharp_rcache_mem_dereg_cb(void *context, ucs_rcache_t *rcache,
                                           ucs_rcache_region_t *rregion)
{
    xccl_sharp_context_t       *ctx    = (xccl_sharp_context_t*) context;
    xccl_sharp_rcache_region_t *region = xccl_derived_of(rregion, xccl_sharp_rcache_region_t);
    void                       *addr   = (void*)region->super.super.start;
    int rc;

    xccl_sharp_debug("RCACHE, mem_dereg, memh %p\n", region->memh);

    rc = sharp_coll_dereg_mr(ctx->sharp_context, region->memh);
    if (rc != SHARP_COLL_SUCCESS) {
        xccl_sharp_error("SHARP deregmr failed\n");
    }
}

static void xccl_sharp_rcache_dump_region_cb(void *context, ucs_rcache_t *rcache,
                                             ucs_rcache_region_t *rregion, char *buf,
                                             size_t max)
{
    xccl_sharp_rcache_region_t *region = xccl_derived_of(rregion, xccl_sharp_rcache_region_t);

    snprintf(buf, max, "memh:%p", region->memh);
}

static ucs_rcache_ops_t xccl_sharp_rcache_ops = {
    .mem_reg     = xccl_sharp_rcache_mem_reg_cb,
    .mem_dereg   = xccl_sharp_rcache_mem_dereg_cb,
    .dump_region = xccl_sharp_rcache_dump_region_cb
};

static xccl_status_t
xccl_sharp_create_context(xccl_team_lib_h lib, xccl_context_config_h config,
                          xccl_tl_context_t **context)
{
    xccl_sharp_context_t        *ctx      = malloc(sizeof(*ctx));
    struct sharp_coll_init_spec init_spec = {0};
    XCCL_CONTEXT_SUPER_INIT(ctx->super, lib, config);

    init_spec.progress_func                  = NULL;
    init_spec.world_rank                     = config->oob.rank;
    init_spec.world_local_rank               = 0;
    init_spec.world_size                     = config->oob.size;
    init_spec.enable_thread_support          = 1;
    init_spec.group_channel_idx              = 0;
    init_spec.oob_colls.barrier              = xccl_sharp_oob_barrier;
    init_spec.oob_colls.bcast                = xccl_sharp_oob_bcast;
    init_spec.oob_colls.gather               = xccl_sharp_oob_gather;
    init_spec.oob_ctx                        = &ctx->super.cfg->oob;
    init_spec.config                         = sharp_coll_default_config;
    init_spec.config.user_progress_num_polls = 1000000;
    init_spec.config.ib_dev_list             = "mlx5_0:1";
    init_spec.job_id                         = xccl_sharp_rand();
    xccl_sharp_oob_bcast((void*)&ctx->super.cfg->oob, &init_spec.job_id,
                         sizeof(uint64_t), 0);
    int ret = sharp_coll_init(&init_spec, &ctx->sharp_context);
    if (ret < 0 ) {
        if (config->oob.rank == 0) {
            xccl_sharp_error("Failed to initialize SHARP collectives:%s(%d)"
                             "job ID:%" PRIu64"\n",
                             sharp_coll_strerror(ret), ret, init_spec.job_id);
        }
        free(ctx);
        return XCCL_ERR_NO_MESSAGE;
    }

    ucs_rcache_params_t rcache_params;
    ucs_status_t status;

    rcache_params.region_struct_size = sizeof(xccl_sharp_rcache_region_t);
    rcache_params.alignment          = 64;
    rcache_params.max_alignment      = (size_t)sysconf(_SC_PAGE_SIZE);
    rcache_params.ucm_events         = XCCL_BIT(17) /*TODO: UCM_EVENT_VM_UNMAPPED */;
    rcache_params.ucm_event_priority = 1000;
    rcache_params.context            = (void*)ctx;
    rcache_params.ops                = &xccl_sharp_rcache_ops;

    status = ucs_rcache_create(&rcache_params, "team_sharp", NULL, &ctx->rcache);
    if (status != UCS_OK) {
        sharp_coll_finalize(ctx->sharp_context);
        free(ctx);
        return XCCL_ERR_NO_MESSAGE;
    }

    *context = &ctx->super;
    return XCCL_OK;
}

static xccl_status_t
xccl_sharp_destroy_context(xccl_tl_context_t *context)
{
    xccl_sharp_context_t *team_sharp_ctx =
        xccl_derived_of(context, xccl_sharp_context_t);

    if (team_sharp_ctx->rcache) {
        ucs_rcache_destroy(team_sharp_ctx->rcache);
    }

    if (team_sharp_ctx->sharp_context) {
        sharp_coll_finalize(team_sharp_ctx->sharp_context);
    }

    free(team_sharp_ctx);
    return XCCL_OK;
}

static xccl_status_t
xccl_sharp_team_create_post(xccl_tl_context_t *context,
                            xccl_team_config_h config,
                            xccl_oob_collectives_t oob,
                            xccl_tl_team_t **team)
{
    xccl_sharp_context_t *team_sharp_ctx =
        xccl_derived_of(context, xccl_sharp_context_t);
    xccl_sharp_team_t *team_sharp = malloc(sizeof(*team_sharp));
    struct sharp_coll_comm_init_spec comm_spec;
    int i, ret;
    XCCL_TEAM_SUPER_INIT(team_sharp->super, context, config, oob);

    comm_spec.size              = oob.size;
    comm_spec.rank              = oob.rank;
    comm_spec.group_world_ranks = NULL;
    comm_spec.oob_ctx           = &team_sharp->super.oob;
    ret = sharp_coll_comm_init(team_sharp_ctx->sharp_context, &comm_spec,
                               &team_sharp->sharp_comm);
    if (ret<0) {
        if (oob.rank == 0) {
            xccl_sharp_error("SHARP group create failed:%s(%d)",
                              sharp_coll_strerror(ret), ret);
        }
        free(team_sharp);
        return XCCL_ERR_NO_MESSAGE;
    }
    *team = &team_sharp->super;
    return XCCL_OK;
}

static xccl_status_t xccl_sharp_team_create_test(xccl_tl_team_t *team)
{
    /*TODO implement true non-blocking */
    return XCCL_OK;
}

static xccl_status_t xccl_sharp_team_destroy(xccl_tl_team_t *team)
{
    xccl_sharp_team_t *team_sharp = xccl_derived_of(team, xccl_sharp_team_t);
    xccl_sharp_context_t *team_sharp_ctx =
        xccl_derived_of(team->ctx, xccl_sharp_context_t);

    sharp_coll_comm_destroy(team_sharp->sharp_comm);
    free(team);
    return XCCL_OK;
}

xccl_team_lib_sharp_t xccl_team_lib_sharp = {
    .super.name                 = "sharp",
    .super.id                   = XCCL_TL_SHARP,
    .super.priority             = 90,
    .super.team_lib_config      = {
        .name                   = "SHARP team library",
        .prefix                 = "TEAM_SHARP_",
        .table                  = xccl_team_lib_sharp_config_table,
        .size                   = sizeof(xccl_team_lib_sharp_config_t),
    },
    .super.params.reproducible  = XCCL_LIB_NON_REPRODUCIBLE,
    .super.params.thread_mode   = XCCL_LIB_THREAD_SINGLE | XCCL_LIB_THREAD_MULTIPLE,
    .super.params.team_usage    = XCCL_USAGE_HW_COLLECTIVES,
    .super.params.coll_types    = XCCL_COLL_CAP_BARRIER | XCCL_COLL_CAP_ALLREDUCE,
    .super.ctx_create_mode      = XCCL_TEAM_LIB_CONTEXT_CREATE_MODE_GLOBAL,
    .super.create_team_context  = xccl_sharp_create_context,
    .super.destroy_team_context = xccl_sharp_destroy_context,
    .super.team_create_post     = xccl_sharp_team_create_post,
    .super.team_create_test     = xccl_sharp_team_create_test,
    .super.team_destroy         = xccl_sharp_team_destroy,
    .super.progress             = NULL,
    .super.team_lib_open        = xccl_sharp_lib_open,
    .super.collective_init      = xccl_sharp_collective_init,
    .super.collective_post      = xccl_sharp_collective_post,
    .super.collective_wait      = xccl_sharp_collective_wait,
    .super.collective_test      = xccl_sharp_collective_test,
    .super.collective_finalize  = xccl_sharp_collective_finalize,
    .super.global_mem_map_start = NULL,
    .super.global_mem_map_test  = NULL,
    .super.global_mem_unmap     = NULL,
};
