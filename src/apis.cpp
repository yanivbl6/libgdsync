/* Copyright (c) 2016, NVIDIA CORPORATION. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *  * Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *  * Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *  * Neither the name of NVIDIA CORPORATION nor the names of its
 *    contributors may be used to endorse or promote products derived
 *    from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#if HAVE_CONFIG_H
#  include <config.h>
#endif /* HAVE_CONFIG_H */

#include <unistd.h>
#include <string.h>
#include <assert.h>

#include <arpa/inet.h>
#include <inttypes.h>

//#include <map>
//#include <algorithm>
//#include <string>
//using namespace std;

//#include <cuda.h>
//#include <infiniband/verbs_exp.h>
//#include <gdrapi.h>

#include "gdsync.h"
#include "gdsync/tools.h"
#include "objs.hpp"
#include "utils.hpp"
#include "memmgr.hpp"
#include "utils.hpp"
#include "archutils.h"
#include "mlnxutils.h"

//-----------------------------------------------------------------------------

static void gds_init_ops(struct peer_op_wr *op, int count)
{
        int i = count;
        while (--i)
                op[i-1].next = &op[i];
        op[count-1].next = NULL;
}

//-----------------------------------------------------------------------------

static void gds_init_send_info(gds_send_request_t *info)
{
        gds_dbg("send_request=%p\n", info);
        memset(info, 0, sizeof(*info));

        info->commit.storage = info->wr;
        info->commit.entries = sizeof(info->wr)/sizeof(info->wr[0]);
        gds_init_ops(info->commit.storage, info->commit.entries);
}

//-----------------------------------------------------------------------------

static void gds_init_wait_request(gds_wait_request_t *request, uint32_t offset)
{
        gds_dbg("wait_request=%p offset=%08x\n", request, offset);
        memset(request, 0, sizeof(*request));
        request->peek.storage = request->wr;
        request->peek.entries = sizeof(request->wr)/sizeof(request->wr[0]);
        request->peek.whence = IBV_EXP_PEER_PEEK_ABSOLUTE;
        request->peek.offset = offset;
        gds_init_ops(request->peek.storage, request->peek.entries);
}

//-----------------------------------------------------------------------------

static int gds_rollback_qp(struct gds_qp *qp, gds_send_request_t * send_info, enum ibv_exp_rollback_flags flag)
{
        struct ibv_exp_rollback_ctx rollback;
        int ret=0;

        assert(qp);
        assert(qp->qp);
        assert(send_info);
        if(
                        flag != IBV_EXP_ROLLBACK_ABORT_UNCOMMITED && 
                        flag != IBV_EXP_ROLLBACK_ABORT_LATE
          )
        {
                gds_err("erroneous ibv_exp_rollback_flags flag input value\n");
                ret=EINVAL;
                goto out;
        } 

        /* from ibv_exp_peer_commit call */
        rollback.rollback_id = send_info->commit.rollback_id;
        /* from ibv_exp_rollback_flag */
        rollback.flags = flag;
        /* Reserved for future expensions, must be 0 */
        rollback.comp_mask = 0;
        gds_warn("Need to rollback WQE %lx\n", rollback.rollback_id);
        ret = ibv_exp_rollback_qp(qp->qp, &rollback);
        if(ret)
                gds_err("error %d in ibv_exp_rollback_qp\n", ret);

out:
        return ret;
}

//-----------------------------------------------------------------------------

#define ntohll(x) (((uint64_t)(ntohl((int)((x << 32) >> 32))) << 32) |  (uint32_t)ntohl(((int)(x >> 32))))

static void gds_dump_swr(const char * func_name, struct gds_swr_info swr_info)
{
    gds_dbg("[%s] wr_id=%lx, num_sge=%d\n", func_name, swr_info.wr_id, swr_info.num_sge);

    for(int j=0; j < swr_info.num_sge; j++)
    {
        gds_dbg("[%s]    SGE=%d, Size ptr=0x%08x, Size=%d (0x%08x), +offset=%d\n", 
            func_name,
            j,
            (uintptr_t)swr_info.sge_list[j].ptr_to_size, 
            (uint32_t) ntohl( ((uint32_t*)swr_info.sge_list[j].ptr_to_size)[0]) ,
            (uint32_t) ((uint32_t*)swr_info.sge_list[j].ptr_to_size)[0],
            ((uint32_t) ntohl( ((uint32_t*)swr_info.sge_list[j].ptr_to_size)[0]) ) + swr_info.sge_list[j].offset );

        gds_dbg("[%s]    SGE=%d, lkey ptr=0x%08x, lkey=%d (0x%08x)\n", 
            func_name,
            j,
            (uintptr_t)swr_info.sge_list[j].ptr_to_lkey, 
            (uint32_t) ntohl( ((uint32_t*)swr_info.sge_list[j].ptr_to_lkey)[0]) ,
            (uint32_t) ((uint32_t*)swr_info.sge_list[j].ptr_to_lkey)[0]);

        gds_dbg("[%s]    SGE=%d, Addr ptr=%lx, Addr=%lx -offset=%lx\n", 
            func_name,
            j,
            (uintptr_t)swr_info.sge_list[j].ptr_to_addr, 
            (uintptr_t)ntohll(((uint64_t*)swr_info.sge_list[j].ptr_to_addr)[0]),
            ((uintptr_t)ntohll(((uint64_t*)swr_info.sge_list[j].ptr_to_addr)[0])) - swr_info.sge_list[j].offset );

        gds_dbg("[%s]    SGE=%d, offset=%lx\n", func_name, j, (uint32_t)swr_info.sge_list[j].offset);
    }
} 

//-----------------------------------------------------------------------------

int gds_post_send(struct gds_qp *qp, gds_send_wr *p_ewr, gds_send_wr **bad_ewr)
{
        int ret = 0, ret_roll=0;
        gds_send_request_t send_info;
        ret = gds_prepare_send(qp, p_ewr, bad_ewr, &send_info);
        if (ret) {
                gds_err("error %d in gds_prepare_send\n", ret);
                goto out;
        }

        ret = gds_post_pokes_on_cpu(1, &send_info, NULL, 0);
        if (ret) {
                gds_err("error %d in gds_post_pokes_on_cpu\n", ret);
                ret_roll = gds_rollback_qp(qp, &send_info, IBV_EXP_ROLLBACK_ABORT_LATE);
                if (ret_roll) {
                        gds_err("error %d in gds_rollback_qp\n", ret_roll);
                }

                goto out;
        }

out:
        return ret;
}

//-----------------------------------------------------------------------------

int gds_post_recv(struct gds_qp *qp, struct ibv_recv_wr *wr, struct ibv_recv_wr **bad_wr)
{
        int ret = 0;

        gds_dbg("qp=%p wr=%p\n", qp, wr);
        assert(qp);
        assert(qp->qp);
        ret = ibv_post_recv(qp->qp, wr, bad_wr);
        if (ret) {
                gds_err("error %d in ibv_post_recv\n", ret);
                goto out;
        }

out:
        return ret;
}

//-----------------------------------------------------------------------------

#define IBV_EXP_SEND_GET_INFO (1 << 28)

int gds_prepare_send(struct gds_qp *qp, gds_send_wr *p_ewr, 
                     gds_send_wr **bad_ewr, 
                     gds_send_request_t *request)
{
        int ret = 0, i = 0;
        bool get_info=false;

        gds_init_send_info(request);
        assert(qp);
        assert(qp->qp);

        if(p_ewr->exp_send_flags & IBV_EXP_SEND_GET_INFO)
            get_info=true;

        if(get_info)
        {
            for(i=0; i < p_ewr->num_sge; i++)
            {
                gds_dbg("SGE#%d, Sending wr %lx with addr=%lx, lkey=%lx length=%d...\n", 
                            i,
                            p_ewr->wr_id, 
                            (uintptr_t)p_ewr->sg_list[i].addr, 
                            (int)p_ewr->sg_list[i].lkey,
                            (int)p_ewr->sg_list[i].length
                );
            }
            memset(&(request->gds_sinfo.swr_info), 0, sizeof(struct gds_swr_info));
        }

        ret = ibv_exp_post_send(qp->qp, p_ewr, bad_ewr);
        if (ret) {
                if (ret == ENOMEM) {
                        // out of space error can happen too often to report
                        gds_dbg("ENOMEM error %d in ibv_exp_post_send\n", ret);
                } else {
                        gds_err("error %d in ibv_exp_post_send\n", ret);
                }
                goto out;
        }
   
 
        if(get_info)
        {
            ret = gds_query_last_info(qp, &(request->gds_sinfo.swr_info));

            if(ret)
            {
                fprintf(stderr, "gds_query_last_info returned %d: %s\n", ret, strerror(ret));
                goto out;
            }

            gds_dump_swr("gds_prepare_send", request->gds_sinfo.swr_info);
        }

        ret = gds_report_post(qp, p_ewr); //increment counter.

        ret = ibv_exp_peer_commit_qp(qp->qp, &request->commit);
        if (ret) {
                gds_err("error %d in ibv_exp_peer_commit_qp\n", ret);
                //gds_wait_kernel();
                goto out;
        }

out:
        return ret;
}

//-----------------------------------------------------------------------------

int gds_prepare_send_info(gds_send_request_t *request,
                        void * ptr_to_size_new, int ptr_to_size_flags,
                        void * ptr_to_lkey_new, int ptr_to_lkey_flags,
                        void * ptr_to_addr_new, int ptr_to_addr_flags)
{
    int ret = 0;
    //This should be an input..
    int sge_index=0;
    CUdeviceptr ptr_to_size_new_d, ptr_to_size_wqe_d;

    assert(request);
    if(
        request->gds_sinfo.swr_info.wr_id == 0 ||
        request->gds_sinfo.swr_info.num_sge <= 0 ||
        request->gds_sinfo.swr_info.sge_list == NULL
    )
    {
        gds_err("Input send request has erroneous wr_id(%lx), num_sge(%d) or sge_list pointer (is NULL=%d)\n", 
            request->gds_sinfo.swr_info.wr_id, request->gds_sinfo.swr_info.num_sge, (request->gds_sinfo.swr_info.sge_list == NULL ? 1 : 0));
        ret=EINVAL;
        goto out;
    }

    //Dummy check if sge_index is not an input...
    if(sge_index > request->gds_sinfo.swr_info.num_sge)
    {
        gds_err("Can't update sge %d: send request has only %d sge\n", sge_index, request->gds_sinfo.swr_info.num_sge);
        ret=EINVAL;
        goto out;
    }

    gds_dbg("new_size=%p, new_lkey=%p, new_addr=%p\n", ptr_to_size_new, ptr_to_lkey_new, ptr_to_addr_new);

    //Size
    request->gds_sinfo.ptr_to_size_wqe_h=request->gds_sinfo.swr_info.sge_list[sge_index].ptr_to_size;
    request->gds_sinfo.ptr_to_size_wqe_d=0;
    request->gds_sinfo.ptr_to_size_new_h=0;
    request->gds_sinfo.ptr_to_size_new_d=0;

    if(ptr_to_size_new != NULL)
    {
        //ptr_to_size is always host memory: we need to pin it
        ret = gds_map_mem(
                            (uint32_t*)request->gds_sinfo.ptr_to_size_wqe_h, 
                            sizeof(uint32_t), GDS_MEMORY_HOST, 
                            &request->gds_sinfo.ptr_to_size_wqe_d
                        );
        if (ret) {
                gds_err("error %d while looking up ptr_to_size_wqe_h=%p\n",
                    ret, (uint32_t*)request->gds_sinfo.ptr_to_size_wqe_h);
                goto out;
        }

        if(memtype_from_flags(ptr_to_size_flags) == GDS_MEMORY_HOST)
            request->gds_sinfo.ptr_to_size_new_h = (uintptr_t) ptr_to_size_new;

        //input can be hostmem or gmem
        ret = gds_map_mem(
                            ptr_to_size_new, 
                            sizeof(uint32_t), memtype_from_flags(ptr_to_size_flags), 
                            &request->gds_sinfo.ptr_to_size_new_d
                        );
        if (ret) {
                gds_err("error %d while looking up ptr_to_size_new=%p\n", ret, ptr_to_size_new);
                goto out;
        }
    }

    //lkey
    request->gds_sinfo.ptr_to_lkey_wqe_h=request->gds_sinfo.swr_info.sge_list[sge_index].ptr_to_lkey;
    request->gds_sinfo.ptr_to_lkey_wqe_d=0;
    request->gds_sinfo.ptr_to_lkey_new_h=0;
    request->gds_sinfo.ptr_to_lkey_new_d=0;

    if(ptr_to_lkey_new != NULL)
    {
        //ptr_to_size is always host memory: we need to pin it
        ret = gds_map_mem(
                            (uint32_t*)request->gds_sinfo.ptr_to_lkey_wqe_h, 
                            sizeof(uint32_t), GDS_MEMORY_HOST, 
                            &request->gds_sinfo.ptr_to_lkey_wqe_d
                        );
        if (ret) {
                gds_err("error %d while looking up ptr_to_lkey_wqe_h=%p\n",
                    ret, (uint32_t*)request->gds_sinfo.ptr_to_lkey_wqe_h);
                goto out;
        }

        if(memtype_from_flags(ptr_to_lkey_flags) == GDS_MEMORY_HOST)
            request->gds_sinfo.ptr_to_lkey_new_h = (uintptr_t) ptr_to_lkey_new;

        //input can be hostmem or gmem
        ret = gds_map_mem(
                            ptr_to_lkey_new, 
                            sizeof(uint32_t), memtype_from_flags(ptr_to_lkey_flags), 
                            &request->gds_sinfo.ptr_to_lkey_new_d
                        );
        if (ret) {
                gds_err("error %d while looking up ptr_to_lkey_new=%p\n", ret, ptr_to_lkey_new);
                goto out;
        }
    }


    //Addr
    request->gds_sinfo.ptr_to_addr_wqe_h=request->gds_sinfo.swr_info.sge_list[sge_index].ptr_to_addr;
    request->gds_sinfo.ptr_to_addr_wqe_d=0;
    request->gds_sinfo.ptr_to_addr_new_h=0;
    request->gds_sinfo.ptr_to_addr_new_d=0;

    if(ptr_to_addr_new != NULL)
    {
        //ptr_to_size is always host memory: we need to pin it
        ret = gds_map_mem(
                            (uint32_t*)request->gds_sinfo.ptr_to_addr_wqe_h, 
                            sizeof(uint32_t), GDS_MEMORY_HOST, 
                            &request->gds_sinfo.ptr_to_addr_wqe_d
                        );
        if (ret) {
                gds_err("error %d while looking up ptr_to_addr_wqe_h=%p\n",
                    ret, (uint32_t*)request->gds_sinfo.ptr_to_addr_wqe_h);
                goto out;
        }

        if(memtype_from_flags(ptr_to_addr_flags) == GDS_MEMORY_HOST)
            request->gds_sinfo.ptr_to_addr_new_h = (uintptr_t) ptr_to_addr_new;

        //input can be hostmem or gmem
        ret = gds_map_mem(
                            ptr_to_addr_new, 
                            sizeof(uint32_t), memtype_from_flags(ptr_to_addr_flags), 
                            &request->gds_sinfo.ptr_to_addr_new_d
                        );
        if (ret) {
                gds_err("error %d while looking up ptr_to_addr_new=%p\n", ret, ptr_to_addr_new);
                goto out;
        }
    }

    gds_err("ptr_to_size_new_h=%lx, ptr_to_size_new_d=%lx, ptr_to_lkey_new_h=%lx, ptr_to_lkey_new_d=%lx, ptr_to_addr_new_h=%lx, ptr_to_addr_new_d=%lx\n",
        request->gds_sinfo.ptr_to_size_new_h, request->gds_sinfo.ptr_to_size_new_d,
        request->gds_sinfo.ptr_to_lkey_new_h, request->gds_sinfo.ptr_to_lkey_new_d,
        request->gds_sinfo.ptr_to_addr_new_h, request->gds_sinfo.ptr_to_addr_new_d);

out:
    return ret;
}

//-----------------------------------------------------------------------------
//SA Model only
int gds_update_send_info(gds_send_request_t *request, int send_flags, CUstream stream)
{
    int ret=0;
    gds_peer *peer = NULL;
    uint32_t mem_type;
    CUresult cures;
    const char * err_str;

    assert(request);

    peer = peer_from_stream(stream);
    if (!peer) {
            return EINVAL;
    }

    gds_dump_swr("gds_update_send_info - before", request->gds_sinfo.swr_info);

    if(request->gds_sinfo.ptr_to_size_wqe_d)
    {
        cures = cuPointerGetAttribute((void *)&mem_type, CU_POINTER_ATTRIBUTE_MEMORY_TYPE, (CUdeviceptr)request->gds_sinfo.ptr_to_size_wqe_d);
        if (cures == CUDA_SUCCESS) {
            if (mem_type == CU_MEMORYTYPE_DEVICE) gds_err("ptr_to_size_wqe_d is GPU mem\n");
            else if (mem_type == CU_MEMORYTYPE_HOST) gds_err("ptr_to_size_wqe_d is host mem\n");
            else
            {
                gds_err("error ptr size mem_type=%d\n", __FUNCTION__, mem_type);
                ret=-1;
                goto out;
            }
        }
        else {
            cuGetErrorString(cures, &err_str);        
            gds_err("%s error ret=%d(%s)\n", __FUNCTION__, cures, err_str);
            ret=-1;
            goto out;
        }        
    }

    if(request->gds_sinfo.ptr_to_lkey_wqe_d)
    {
        cures = cuPointerGetAttribute((void *)&mem_type, CU_POINTER_ATTRIBUTE_MEMORY_TYPE, (CUdeviceptr)request->gds_sinfo.ptr_to_lkey_wqe_d);
        if (cures == CUDA_SUCCESS) {
            if (mem_type == CU_MEMORYTYPE_DEVICE) gds_err("ptr_to_lkey_wqe_d is GPU mem\n");
            else if (mem_type == CU_MEMORYTYPE_HOST) gds_err("ptr_to_lkey_wqe_d is host mem\n");
            else
            {
                gds_err("error ptr size mem_type=%d\n", __FUNCTION__, mem_type);
                ret=-1;
                goto out;
            }
        }
        else {
            cuGetErrorString(cures, &err_str);        
            gds_err("%s error ret=%d(%s)\n", __FUNCTION__, cures, err_str);
            ret=-1;
            goto out;
        }        
    }

    if(request->gds_sinfo.ptr_to_addr_wqe_d)
    {
        cures = cuPointerGetAttribute((void *)&mem_type, CU_POINTER_ATTRIBUTE_MEMORY_TYPE, (CUdeviceptr)request->gds_sinfo.ptr_to_addr_wqe_d);
        if (cures == CUDA_SUCCESS) {
            if (mem_type == CU_MEMORYTYPE_DEVICE) gds_err("ptr_to_addr_wqe_d is GPU mem\n");
            else if (mem_type == CU_MEMORYTYPE_HOST) gds_err("ptr_to_addr_wqe_d is host mem\n");
            else
            {
                gds_err("error ptr size mem_type=%d\n", __FUNCTION__, mem_type);
                ret=-1;
                goto out;
            }
        }
        else {
            cuGetErrorString(cures, &err_str);        
            gds_err("%s error ret=%d(%s)\n", __FUNCTION__, cures, err_str);
            ret=-1;
            goto out;
        }        
    }

#if 0
    gds_err("ptr_to_size_wqe_h=0x%lx ptr_to_size_wqe_d=0x%lx (val=0x%08x), ptr_to_size_new_h=0x%lx ptr_to_size_new_d=0x%lx (val=0x%08x)\n",
        request->gds_sinfo.ptr_to_size_wqe_h, request->gds_sinfo.ptr_to_size_wqe_d, ((uint32_t*)(request->gds_sinfo.ptr_to_size_wqe_h))[0],
        request->gds_sinfo.ptr_to_size_new_h, request->gds_sinfo.ptr_to_size_new_d, ((uint32_t*)(request->gds_sinfo.ptr_to_size_new_h))[0]
    );
    
    gds_err("ptr_to_addr_wqe_h=0x%lx ptr_to_addr_wqe_d=0x%lx (val=0x%llx), ptr_to_addr_new_h=0x%lx ptr_to_addr_new_d=0x%lx (val=0x%llx)\n",
        request->gds_sinfo.ptr_to_addr_wqe_h, request->gds_sinfo.ptr_to_addr_wqe_d, ((uintptr_t*)(request->gds_sinfo.ptr_to_addr_wqe_h))[0],
        request->gds_sinfo.ptr_to_addr_new_h, request->gds_sinfo.ptr_to_addr_new_d, ((uintptr_t*)(request->gds_sinfo.ptr_to_addr_new_h))[0]
    );
#endif

    ret = gds_launch_update_send_params(
                                    peer,
                                    request->gds_sinfo.ptr_to_size_wqe_d, request->gds_sinfo.ptr_to_size_new_d,
                                    request->gds_sinfo.ptr_to_lkey_wqe_d, request->gds_sinfo.ptr_to_lkey_new_d,
                                    request->gds_sinfo.ptr_to_addr_wqe_d, request->gds_sinfo.ptr_to_addr_new_d,
                                    stream);
    if(ret)
    {
        gds_err("gds_launch_update_send_params error %d\n", ret);
        goto out;
    }

    if(send_flags & GDS_SYNC)
    {
        cuStreamSynchronize(stream);
        gds_dump_swr("gds_update_send_info - after", request->gds_sinfo.swr_info);
    }


out:
    return ret;
}

//-----------------------------------------------------------------------------

int gds_stream_queue_send(CUstream stream, struct gds_qp *qp, gds_send_wr *p_ewr, gds_send_wr **bad_ewr)
{
        int ret = 0, ret_roll = 0;
        gds_send_request_t send_info;
        gds_descriptor_t descs[1];

        assert(qp);
        assert(p_ewr);

        ret = gds_prepare_send(qp, p_ewr, bad_ewr, &send_info);
        if (ret) {
                gds_err("error %d in gds_prepare_send\n", ret);
                goto out;
        }

        descs[0].tag = GDS_TAG_SEND;
        descs[0].send = &send_info;

        ret=gds_stream_post_descriptors(stream, 1, descs, 0);
        if (ret) {
                gds_err("error %d in gds_stream_post_descriptors\n", ret);
                goto out;
        }

        out:
        return ret;
}

//-----------------------------------------------------------------------------

int gds_stream_post_send(CUstream stream, gds_send_request_t *request)
{
        int ret = 0;
        ret = gds_stream_post_send_all(stream, 1, request);
        if (ret) {
                gds_err("gds_stream_post_send_all (%d)\n", ret);
        }
        return ret;
}

//-----------------------------------------------------------------------------

int gds_stream_post_send_all(CUstream stream, int count, gds_send_request_t *request)
{
        int ret = 0, k = 0;
        gds_descriptor_t * descs = NULL;

        assert(request);
        assert(count);

        descs = (gds_descriptor_t *) calloc(count, sizeof(gds_descriptor_t));
        if(!descs)
        {
                gds_err("Calloc for %d elements\n", count);
                ret=ENOMEM;
                goto out;
        }

        for (k=0; k<count; k++) {
                descs[k].tag = GDS_TAG_SEND;
                descs[k].send = &request[k];
        }

        ret=gds_stream_post_descriptors(stream, count, descs, 0);
        if (ret) {
                gds_err("error %d in gds_stream_post_descriptors\n", ret);
                goto out;
        }

        out:
            if(descs) free(descs);
            return ret;
}

//-----------------------------------------------------------------------------

int gds_prepare_wait_cq(struct gds_cq *cq, gds_wait_request_t *request, int flags)
{
        int retcode = 0;
        if (flags != 0) {
                gds_err("invalid flags != 0\n");
                return EINVAL;
        }

        gds_init_wait_request(request, cq->curr_offset++);

        retcode = ibv_exp_peer_peek_cq(cq->cq, &request->peek);
        if (retcode == -ENOSPC) {
                // TODO: handle too few entries
                gds_err("not enough ops in peer_peek_cq\n");
                goto out;
        } else if (retcode) {
                gds_err("error %d in peer_peek_cq\n", retcode);
                goto out;
        }
        //gds_dump_wait_request(request, 1);
        out:
	       return retcode;
}

//-----------------------------------------------------------------------------

int gds_append_wait_cq(gds_wait_request_t *request, uint32_t *dw, uint32_t val)
{
        int ret = 0;
        unsigned MAX_NUM_ENTRIES = sizeof(request->wr)/sizeof(request->wr[0]);
        unsigned n = request->peek.entries;
        struct peer_op_wr *wr = request->peek.storage;

        if (n + 1 > MAX_NUM_ENTRIES) {
            gds_err("no space left to stuff a poke\n");
            ret = ENOMEM;
            goto out;
        }

        // at least 1 op
        assert(n);
        assert(wr);

        for (; n; --n) wr = wr->next;
        assert(wr);

        wr->type = IBV_EXP_PEER_OP_STORE_DWORD;
        wr->wr.dword_va.data = val;
        wr->wr.dword_va.target_id = 0; // direct mapping, offset IS the address
        wr->wr.dword_va.offset = (ptrdiff_t)(dw-(uint32_t*)0);

        ++request->peek.entries;

        out:
        return ret;
}

//-----------------------------------------------------------------------------

int gds_stream_post_wait_cq(CUstream stream, gds_wait_request_t *request)
{
	return gds_stream_post_wait_cq_multi(stream, 1, request, NULL, 0);
}

//-----------------------------------------------------------------------------

int gds_stream_post_wait_cq_all(CUstream stream, int count, gds_wait_request_t *requests)
{
	return gds_stream_post_wait_cq_multi(stream, count, requests, NULL, 0);
}

//-----------------------------------------------------------------------------

static int gds_abort_wait_cq(struct gds_cq *cq, gds_wait_request_t *request)
{
        assert(cq);
        assert(request);
        struct ibv_exp_peer_abort_peek abort_ctx;
        abort_ctx.peek_id = request->peek.peek_id;
        abort_ctx.comp_mask = 0;
        return ibv_exp_peer_abort_peek_cq(cq->cq, &abort_ctx);
}

//-----------------------------------------------------------------------------

int gds_stream_wait_cq(CUstream stream, struct gds_cq *cq, int flags)
{
        int retcode = 0;
        int ret;
        gds_wait_request_t request;

        assert(cq);
        assert(stream);

        if (flags) {
                retcode = EINVAL;
                goto out;
        }

        ret = gds_prepare_wait_cq(cq, &request, flags);
        if (ret) {
                gds_err("error %d in gds_prepare_wait_cq\n", ret);
                goto out;
        }

        ret = gds_stream_post_wait_cq(stream, &request);
        if (ret) {
                gds_err("error %d in gds_stream_post_wait_cq_ex\n", ret);
                int retcode2 = gds_abort_wait_cq(cq, &request);
                if (retcode2) {
                        gds_err("nested error %d while aborting request\n", retcode2);
                }
                retcode = ret;
                goto out;
        }

out:
	return retcode;
}

//-----------------------------------------------------------------------------

int gds_post_wait_cq(struct gds_cq *cq, gds_wait_request_t *request, int flags)
{
        int retcode = 0;

        if (flags) {
                retcode = EINVAL;
                goto out;
        }

        retcode = gds_abort_wait_cq(cq, request);
out:
        return retcode;
}

//-----------------------------------------------------------------------------

int gds_prepare_wait_value32(gds_wait_value32_t *desc, uint32_t *ptr, uint32_t value, gds_wait_cond_flag_t cond_flags, int flags)
{
        int ret = 0;
        assert(desc);

        gds_dbg("desc=%p ptr=%p value=0x%08x cond_flags=0x%x flags=0x%x\n",
                desc, ptr, value, cond_flags, flags);

        if (flags & ~(GDS_WAIT_POST_FLUSH_REMOTE|GDS_MEMORY_MASK)) {
                gds_err("invalid flags\n");
                ret = EINVAL;
                goto out;
        }
        if (!is_valid(memtype_from_flags(flags))) {
                gds_err("invalid memory type in flags\n");
                ret = EINVAL;
                goto out;
        }
        if (!is_valid(cond_flags)) {
                gds_err("invalid cond flags\n");
                ret = EINVAL;
                goto out;
        }
        desc->ptr = ptr;
        desc->value = value;
        desc->flags = flags;
        desc->cond_flags = cond_flags;
out:
        return ret;
}

//-----------------------------------------------------------------------------

int gds_prepare_write_value32(gds_write_value32_t *desc, uint32_t *ptr, uint32_t value, int flags)
{
        int ret = 0;
        assert(desc);
        if (!is_valid(memtype_from_flags(flags))) {
                gds_err("invalid memory type in flags\n");
                ret = EINVAL;
                goto out;
        }
        if (flags & ~(GDS_WRITE_PRE_BARRIER_SYS|GDS_MEMORY_MASK)) {
                gds_err("invalid flags\n");
                ret = EINVAL;
                goto out;
        }
        desc->ptr = ptr;
        desc->value = value;
        desc->flags = flags;
out:
        return ret;
}

//-----------------------------------------------------------------------------

int gds_prepare_write_memory(gds_write_memory_t *desc, uint8_t *dest, const uint8_t *src, size_t count, int flags)
{
        int ret = 0;
        assert(desc);
        if (!is_valid(memtype_from_flags(flags))) {
                gds_err("invalid memory type in flags\n");
                ret = EINVAL;
                goto out;
        }
        if (flags & ~(GDS_WRITE_MEMORY_POST_BARRIER_SYS|GDS_WRITE_MEMORY_PRE_BARRIER_SYS|GDS_MEMORY_MASK)) {
                gds_err("invalid flags\n");
                ret = EINVAL;
                goto out;
        }
        desc->dest = dest;
        desc->src = src;
        desc->count = count;
        desc->flags = flags;
out:
        return ret;
}

//-----------------------------------------------------------------------------

static bool no_network_descs_after_entry(size_t n_descs, gds_descriptor_t *descs, size_t idx)
{
        bool ret = true;
        size_t i;
        for(i = idx+1; i < n_descs; ++i) {
                gds_descriptor_t *desc = descs + i;
                switch(desc->tag) {
                case GDS_TAG_SEND:
                case GDS_TAG_WAIT:
                        ret = false;
                        goto out;
                case GDS_TAG_WAIT_VALUE32:
                case GDS_TAG_WRITE_VALUE32:
                        break;
                default:
                        gds_err("invalid tag\n");
                        ret = EINVAL;
                        goto out;
                }
        }
out:
        return ret;
}

static int get_wait_info(size_t n_descs, gds_descriptor_t *descs, size_t &n_waits, size_t &last_wait)
{
        int ret = 0;
        size_t i;
        for(i = 0; i < n_descs; ++i) {
                gds_descriptor_t *desc = descs + i;
                switch(desc->tag) {
                case GDS_TAG_WAIT:
                        ++n_waits;
                        last_wait = i;
                        break;
                case GDS_TAG_SEND:
                case GDS_TAG_WAIT_VALUE32:
                case GDS_TAG_WRITE_VALUE32:
                case GDS_TAG_WRITE_MEMORY:
                        break;
                default:
                        gds_err("invalid tag\n");
                        ret = EINVAL;
                }
        }
        return ret;
}

static int calc_n_mem_ops(size_t n_descs, gds_descriptor_t *descs, size_t &n_mem_ops)
{
        int ret = 0;
        n_mem_ops = 0;
        size_t i;
        for(i = 0; i < n_descs; ++i) {
                gds_descriptor_t *desc = descs + i;
                switch(desc->tag) {
                case GDS_TAG_SEND:
                        n_mem_ops += desc->send->commit.entries + 2; // extra space, ugly
                        break;
                case GDS_TAG_WAIT:
                        n_mem_ops += desc->wait->peek.entries + 2; // ditto
                        break;
                case GDS_TAG_WAIT_VALUE32:
                case GDS_TAG_WRITE_VALUE32:
                case GDS_TAG_WRITE_MEMORY:
                        n_mem_ops += 2; // ditto
                        break;
                default:
                        gds_err("invalid tag\n");
                        ret = EINVAL;
                }
        }
        return ret;
}

static bool gds_can_use_opt_kernel(size_t n_descs, gds_descriptor_t *descs, size_t &n_mem_ops)
{
        return false;
        size_t i;
        gds_tag_t tags[] = { GDS_TAG_SEND, GDS_TAG_WAIT, GDS_TAG_WAIT };
        for(i = 0; i < n_descs; ++i) {
                gds_descriptor_t *desc = descs + i;
                if ((i >= sizeof(tags)/sizeof(tags[0])) || (desc->tag != tags[i])) {
                        return false;
                }
        }
        return true;
}

int gds_stream_post_descriptors(CUstream stream, size_t n_descs, gds_descriptor_t *descs, int flags)
{
        size_t i;
        int idx = 0;
        int ret = 0;
        int retcode = 0;
        size_t n_mem_ops = 0;
        size_t n_waits = 0;
        size_t last_wait = 0;
        bool move_flush = false;
        gds_peer *peer = NULL;
        gds_op_list_t params;
        bool use_opt = false;

        ret = calc_n_mem_ops(n_descs, descs, n_mem_ops);
        if (ret) {
                gds_err("error %d in calc_n_mem_ops\n", ret);
                goto out;
        }

        ret = get_wait_info(n_descs, descs, n_waits, last_wait);
        if (ret) {
                gds_err("error %d in get_wait_info\n", ret);
                goto out;
        }

        gds_dbg("n_descs=%zu n_waits=%zu n_mem_ops=%zu\n", n_descs, n_waits, n_mem_ops);

        // move flush to last wait in the whole batch
        if (n_waits && no_network_descs_after_entry(n_descs, descs, last_wait)) {
                gds_dbg("optimizing FLUSH to last wait i=%zu\n", last_wait);
                move_flush = true;
        }
        // alternatively, remove flush for wait is next op is a wait too

        use_opt = gds_can_use_opt_kernel(n_descs, descs, n_mem_ops);

        peer = peer_from_stream(stream);
        if (!peer) {
                return EINVAL;
        }

        for(i = 0; i < n_descs; ++i) {
                gds_descriptor_t *desc = descs + i;
                switch(desc->tag) {
                case GDS_TAG_SEND: {
                        gds_send_request_t *sreq = desc->send;
                        retcode = gds_post_ops(peer, sreq->commit.entries, sreq->commit.storage, params);
                        if (retcode) {
                                gds_err("error %d in gds_post_ops\n", retcode);
                                ret = retcode;
                                goto out;
                        }
                        break;
                }
                case GDS_TAG_WAIT: {
                        gds_wait_request_t *wreq = desc->wait;
                        int flags = 0;
                        if (move_flush && i != last_wait) {
                                gds_dbg("discarding FLUSH!\n");
                                flags = GDS_POST_OPS_DISCARD_WAIT_FLUSH;
                        }
                        retcode = gds_post_ops(peer, wreq->peek.entries, wreq->peek.storage, params, flags);
                        if (retcode) {
                                gds_err("error %d in gds_post_ops\n", retcode);
                                ret = retcode;
                                goto out;
                        }
                        break;
                }
                case GDS_TAG_WAIT_VALUE32:
                        retcode = gds_fill_poll(peer, params, desc->wait32.ptr, desc->wait32.value, desc->wait32.cond_flags, desc->wait32.flags);
                        if (retcode) {
                                gds_err("error %d in gds_fill_poll\n", retcode);
                                ret = retcode;
                                goto out;
                        }
                        break;
                case GDS_TAG_WRITE_VALUE32:
                        retcode = gds_fill_poke(peer, params, desc->write32.ptr, desc->write32.value, desc->write32.flags);
                        if (retcode) {
                                gds_err("error %d in gds_fill_poke\n", retcode);
                                ret = retcode;
                                goto out;
                        }
                        break;
                case GDS_TAG_WRITE_MEMORY:
                        retcode = gds_fill_inlcpy(peer, params, desc->writemem.dest, desc->writemem.src, desc->writemem.count, desc->writemem.flags);
                        if (retcode) {
                                gds_err("error %d in gds_fill_inlcpy\n", retcode);
                                ret = retcode;
                                goto out;
                        }
                        break;
                default:
                        gds_err("invalid tag for %zu entry\n", i);
                        ret = EINVAL;
                        goto out;
                        break;
                }
        }

        if (use_opt)
                retcode = gds_launch_1QPSend_2CQWait(peer, stream, params);
        else
                retcode = gds_stream_batch_ops(peer, stream, params, 0);

        if (retcode) {
                gds_err("error %d in gds_stream_batch_ops\n", retcode);
                ret = retcode;
                goto out;
        }

out:
        return ret;
}

//-----------------------------------------------------------------------------

int gds_post_descriptors(size_t n_descs, gds_descriptor_t *descs, int flags)
{
        size_t i;
        int ret = 0;
        int retcode = 0;
        for(i = 0; i < n_descs; ++i) {
                gds_descriptor_t *desc = descs + i;
                switch(desc->tag) {
                case GDS_TAG_SEND: {
                        gds_dbg("desc[%zu] SEND\n", i);
                        gds_send_request_t *sreq = desc->send;
                        retcode = gds_post_ops_on_cpu(sreq->commit.entries, sreq->commit.storage, flags);
                        if (retcode) {
                                gds_err("error %d in gds_post_ops_on_cpu\n", retcode);
                                ret = retcode;
                                goto out;
                        }
                        break;
                }
                case GDS_TAG_WAIT: {
                        gds_dbg("desc[%zu] WAIT\n", i);
                        gds_wait_request_t *wreq = desc->wait;
                        retcode = gds_post_ops_on_cpu(wreq->peek.entries, wreq->peek.storage, flags);
                        if (retcode) {
                                gds_err("error %d in gds_post_ops_on_cpu\n", retcode);
                                ret = retcode;
                                goto out;
                        }
                        break;
                }
                case GDS_TAG_WAIT_VALUE32: {
                        gds_dbg("desc[%zu] WAIT_VALUE32\n", i);
                        uint32_t *ptr = desc->wait32.ptr;
                        uint32_t value = desc->wait32.value;
                        bool flush = false;
                        if (desc->wait32.flags & GDS_WAIT_POST_FLUSH_REMOTE) {
                                gds_err("GDS_WAIT_POST_FLUSH_REMOTE flag is not supported yet\n");
                                flush = true;
                        }
                        gds_memory_type_t mem_type = (gds_memory_type_t)(desc->wait32.flags & GDS_MEMORY_MASK);
                        switch(mem_type) {
                        case GDS_MEMORY_GPU:
                                // dereferencing ptr may fail if ptr points to CUDA device memory
                        case GDS_MEMORY_HOST:
                        case GDS_MEMORY_IO:
                                break;
                        default:
                                gds_err("invalid memory type 0x%02x in WAIT_VALUE32\n", mem_type);
                                ret = EINVAL;
                                goto out;
                                break;
                        }
                        bool done = false;
                        do {
                                uint32_t data = gds_atomic_get(ptr);
                                switch(desc->wait32.cond_flags) {
                                case GDS_WAIT_COND_GEQ:
                                        done = ((int32_t)data - (int32_t)value >= 0);
                                        break;
                                case GDS_WAIT_COND_EQ:
                                        done = (data == value);
                                        break;
                                case GDS_WAIT_COND_AND:
                                        done = (data & value);
                                        break;
                                case GDS_WAIT_COND_NOR:
                                        done = ~(data | value);
                                        break;
                                default:
                                        gds_err("invalid condition flags 0x%02x in WAIT_VALUE32\n", desc->wait32.cond_flags);
                                        goto out;
                                        break;
                                }
                                if (done)
                                        break;
                                // TODO: more aggressive CPU relaxing needed here to avoid starving I/O fabric
                                arch_cpu_relax();
                        } while(true);
                        break;
                }
                case GDS_TAG_WRITE_VALUE32: {
                        gds_dbg("desc[%zu] WRITE_VALUE32\n", i);
                        uint32_t *ptr = desc->write32.ptr;
                        uint32_t value = desc->write32.value;
                        gds_memory_type_t mem_type = (gds_memory_type_t)(desc->write32.flags & GDS_MEMORY_MASK);
                        switch(mem_type) {
                        case GDS_MEMORY_GPU:
                                // dereferencing ptr may fail if ptr points to CUDA device memory
                        case GDS_MEMORY_HOST:
                        case GDS_MEMORY_IO:
                                break;
                        default:
                                gds_err("invalid memory type 0x%02x in WRITE_VALUE32\n", mem_type);
                                ret = EINVAL;
                                goto out;
                                break;
                        }
                        bool barrier = (desc->write32.flags & GDS_WRITE_PRE_BARRIER_SYS);
                        if (barrier)
                                wmb();
                        gds_atomic_set(ptr, value);
                        break;
                }
                case GDS_TAG_WRITE_MEMORY: {
                        void *dest = desc->writemem.dest;
                        const void *src = desc->writemem.src;
                        size_t nbytes = desc->writemem.count;
                        bool barrier = (desc->writemem.flags & GDS_WRITE_MEMORY_POST_BARRIER_SYS);
                        gds_memory_type_t mem_type = memtype_from_flags(desc->writemem.flags);
                        gds_dbg("desc[%zu] WRITE_MEMORY dest=%p src=%p len=%zu memtype=%02x\n", i, dest, src, nbytes, mem_type);
                        switch(mem_type) {
                        case GDS_MEMORY_GPU:
                        case GDS_MEMORY_HOST:
                                memcpy(dest, src, nbytes);
                                break;
                        case GDS_MEMORY_IO:
                                assert(nbytes % sizeof(uint64_t));
                                assert(((unsigned long)dest & 0x7) == 0);
                                gds_bf_copy((uint64_t*)dest, (uint64_t*)src, nbytes);
                                break;
                        default:
                                assert(!"invalid mem type");
                                break;
                        }
                        if (barrier)
                                wmb();
                        break;
                }
                default:
                        gds_err("invalid tag for %zu entry\n", i);
                        ret = EINVAL;
                        goto out;
                        break;
                }
        }
out:
        return ret;
}

struct mlx5_send_wqe{
    uint32_t ctrl1;
    uint32_t qpn_ds;
    uint64_t ctrl34;
    uint64_t send12;
    uint64_t send34;
    struct mlx5_sge sge;
}

struct mlx5_sge{
    uint32_t byte_count;
    uint32_t key;
    uint64_t addr;
}

int gds_report_post(struct gds_qp *qp  /*, struct gds_send_wr* wr*/){
    ++(qp->swq_cnt);
    return 0;
    /*//Smarter Alternative:
    struct mlx5_send_wqe* wqe = (struct mlx5_send_wqe*)  ((char*) qp->swq + qp->swq_stride * ((qp->swq_cnt) % qp->swq_size));
    size_t ds = (ntohl(wqe->qpn_ds) & (0x0000007f));
    size_t wqes_per_block = (qp->swq_stride / sizeof(mlx5_sge));
    size_t num_blocks = ds / wqes_per_block + !!(ds % wqes_per_block);
    (qp->swq_cnt)+=num_blocks;
    return 0;
    */
}

int gds_query_last_info(struct gds_qp *qp, struct gds_swr_info* gds_info){
    struct mlx5_send_wqe* wqe = (struct mlx5_send_wqe*)  ((char*) qp->swq + qp->swq_stride * ((qp->swq_cnt) % qp->swq_size));
    gds_info->num_sge = (ntohl(wqe->qpn_ds) & (0x0000007f)) - 2;
    struct mlx5_sge* sge = &(wqe->sge);
    size_t wqes_per_block = (qp->swq_stride / sizeof(mlx5_sge)); 
    for (size_t i = 0; i< gds_info->num_sge; ++i){
        gds_info->sge_list[i].ptr_to_size = &(sge->byte_count);
        gds_info->sge_list[i].ptr_to_lkey = &(sge->key);
        gds_info->sge_list[i].ptr_to_addr = &(sge->addr);
        (++sge);
        //TODO: handle wrap around (I Think commented should work but lets handle it later)
        //if (qp->swq_cnt + (i / wqes_per_block + !!(i % wqes_per_block)) > qp->swq_size) sge = (struct mlx5_sge*) qp->swq;
    }
    gds_info->wr_id = 1;  //just exists to match old API.
    return 0;
}

//-----------------------------------------------------------------------------

/*
 * Local variables:
 *  c-indent-level: 8
 *  c-basic-offset: 8
 *  tab-width: 8
 *  indent-tabs-mode: nil
 * End:
 */
