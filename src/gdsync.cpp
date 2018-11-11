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

#include <gdsync.h>
#include <gdsync/tools.h>

#include "utils.hpp"
#include "memmgr.hpp"
#include "mem.hpp"
#include "objs.hpp"
#include "archutils.h"
#include "mlnxutils.h"
#include "task_queue.hpp"

extern "C" {
#include <infiniband/mlx5dv.h>
}

//-----------------------------------------------------------------------------

void gds_assert(const char *cond, const char *file, unsigned line, const char *function)
{
        gds_err("assertion '%s' failed in %s at %s:%d\n", cond, function, file, line);
        abort();
}

int gds_dbg_enabled()
{
        static int gds_dbg_is_enabled = -1;
        if (-1 == gds_dbg_is_enabled) {
                const char *env = getenv("GDS_ENABLE_DEBUG");
                if (env) {
                        int en = atoi(env);
                        gds_dbg_is_enabled = !!en;
                        //printf("GDS_ENABLE_DEBUG=%s\n", env);
                } else
                        gds_dbg_is_enabled = 0;
        }
        return gds_dbg_is_enabled;
}

#if 0
int gds_flusher_enabled()
{
    static int gds_flusher_is_enabled = -1;
    if (-1 == gds_flusher_is_enabled) {
        const char *env = getenv("GDS_ENABLE_FLUSHER");
        if (env) {
            int en = atoi(env);
            gds_flusher_is_enabled = !!en;
        } else
            gds_flusher_is_enabled = 0;
    
        gds_warn("GDS_ENABLE_FLUSHER=%d\n", gds_flusher_is_enabled);
    }
    return gds_flusher_is_enabled;
}
#endif

//-----------------------------------------------------------------------------
// detect Async APIs

#if !HAVE_DECL_CU_STREAM_BATCH_MEM_OP_RELAXED_ORDERING
#define CU_STREAM_BATCH_MEM_OP_RELAXED_ORDERING (1<<30)
#endif

// TODO: use correct value
// TODO: make it dependent upon the particular GPU
const size_t GDS_GPU_MAX_INLINE_SIZE = 256;

//-----------------------------------------------------------------------------

// Note: these are default overrides, i.e. allow to disable/enable the features
// in case the GPU supports them

static bool gds_enable_write64()
{
        static int gds_disable_write64 = -1;
        if (-1 == gds_disable_write64) {
                const char *env = getenv("GDS_DISABLE_WRITE64");
                if (env)
                        gds_disable_write64 = !!atoi(env);
                else
                        gds_disable_write64 = 0;
                gds_dbg("GDS_DISABLE_WRITE64=%d\n", gds_disable_write64);
        }
        return !gds_disable_write64;
}

static bool gds_enable_wait_nor()
{
        static int gds_disable_wait_nor = -1;
        if (-1 == gds_disable_wait_nor) {
                const char *env = getenv("GDS_DISABLE_WAIT_NOR");
                if (env)
                        gds_disable_wait_nor = !!atoi(env);
                else
                        gds_disable_wait_nor = 1; // WAR for issue #68
                gds_dbg("GDS_DISABLE_WAIT_NOR=%d\n", gds_disable_wait_nor);
        }
        return !gds_disable_wait_nor;
}

static bool gds_enable_remote_flush()
{
        static int gds_disable_remote_flush = -1;
        if (-1 == gds_disable_remote_flush) {
                const char *env = getenv("GDS_DISABLE_REMOTE_FLUSH");
                if (env)
                        gds_disable_remote_flush = !!atoi(env);
                else
                        gds_disable_remote_flush = 0;
                gds_dbg("GDS_DISABLE_REMOTE_FLUSH=%d\n", gds_disable_remote_flush);
        }
        return !gds_disable_remote_flush;
}

static bool gds_enable_wait_checker()
{
        static int gds_enable_wait_checker = -1;
        if (-1 == gds_enable_wait_checker) {
                const char *env = getenv("GDS_ENABLE_WAIT_CHECKER");
                if (env)
                        gds_enable_wait_checker = !!atoi(env);
                else
                        gds_enable_wait_checker = 0;
                gds_dbg("GDS_ENABLE_WAIT_CHECKER=%d\n", gds_enable_wait_checker);
        }
        return gds_enable_wait_checker;
}

static bool gds_enable_inlcpy()
{
        static int gds_disable_inlcpy = -1;
        if (-1 == gds_disable_inlcpy) {
                const char *env = getenv("GDS_DISABLE_WRITEMEMORY");
                if (env)
                        gds_disable_inlcpy = !!atoi(env);
                else
                        gds_disable_inlcpy = 0;
                gds_dbg("GDS_DISABLE_WRITEMEMORY=%d\n", gds_disable_inlcpy);
        }
        return !gds_disable_inlcpy;
}

// simulate 64-bits writes with inlcpy
static bool gds_simulate_write64()
{
        static int gds_simulate_write64 = -1;
        if (-1 == gds_simulate_write64) {
                const char *env = getenv("GDS_SIMULATE_WRITE64");
                if (env)
                        gds_simulate_write64 = !!atoi(env);
                else
                        gds_simulate_write64 = 0; // default
                gds_dbg("GDS_SIMULATE_WRITE64=%d\n", gds_simulate_write64);

                if (gds_simulate_write64 && gds_enable_inlcpy()) {
                        gds_warn("WRITEMEMORY has priority over SIMULATE_WRITE64, using the former\n");
                        gds_simulate_write64 = 0;
                }
        }

        return gds_simulate_write64;
}

static bool gds_enable_membar()
{
        static int gds_disable_membar = -1;
        if (-1 == gds_disable_membar) {
                const char *env = getenv("GDS_DISABLE_MEMBAR");
                if (env)
                        gds_disable_membar = !!atoi(env);
                else
                        gds_disable_membar = 0;
                gds_dbg("GDS_DISABLE_MEMBAR=%d\n", gds_disable_membar);
        }
        return !gds_disable_membar;
}

static bool gds_enable_weak_consistency()
{
        static int gds_disable_weak_consistency = -1;
        if (-1 == gds_disable_weak_consistency) {
                const char *env = getenv("GDS_DISABLE_WEAK_CONSISTENCY");
                if (env)
                        gds_disable_weak_consistency = !!atoi(env);
                else
                        gds_disable_weak_consistency = 1; // disabled by default
                gds_dbg("GDS_DISABLE_WEAK_CONSISTENCY=%d\n", gds_disable_weak_consistency);
        }
        gds_dbg("gds_disable_weak_consistency=%d\n",
                gds_disable_weak_consistency);
        return !gds_disable_weak_consistency;
}

//-----------------------------------------------------------------------------

static bool gds_enable_dump_memops()
{
        static int gds_enable_dump_memops = -1;
        if (-1 == gds_enable_dump_memops) {
            const char *env = getenv("GDS_ENABLE_DUMP_MEMOPS");
            if (env)
                    gds_enable_dump_memops = !!atoi(env);
            else
                    gds_enable_dump_memops = 0; // disabled by default
            gds_dbg("GDS_ENABLE_DUMP_MEMOPS=%d\n", gds_enable_dump_memops);
        }
        return gds_enable_dump_memops;
}

//-----------------------------------------------------------------------------

void gds_dump_param(CUstreamBatchMemOpParams *param)
{
        switch(param->operation) {
        case CU_STREAM_MEM_OP_WAIT_VALUE_32:
                gds_info("WAIT32 addr:%p alias:%p value:%08x flags:%08x\n",
                        (void*)param->waitValue.address,
                        (void*)param->writeValue.alias,
                        param->waitValue.value,
                        param->waitValue.flags);
                break;

        case CU_STREAM_MEM_OP_WRITE_VALUE_32:
                gds_info("WRITE32 addr:%p alias:%p value:%08x flags:%08x\n",
                        (void*)param->writeValue.address,
                        (void*)param->writeValue.alias,
                        param->writeValue.value,
                        param->writeValue.flags);
                break;

        case CU_STREAM_MEM_OP_FLUSH_REMOTE_WRITES:
                gds_dbg("FLUSH\n");
                break;

#if HAVE_DECL_CU_STREAM_MEM_OP_WRITE_MEMORY
        case CU_STREAM_MEM_OP_WRITE_MEMORY:
                gds_info("INLINECOPY addr:%p alias:%p src:%p len=%zu flags:%08x\n",
                        (void*)param->writeMemory.dst,
                        (void*)param->writeMemory.alias,
                        (void*)param->writeMemory.src,
                        param->writeMemory.byteCount,
                        param->writeMemory.flags);
                break;
#endif

#if HAVE_DECL_CU_STREAM_MEM_OP_MEMORY_BARRIER
        case CU_STREAM_MEM_OP_MEMORY_BARRIER:
                gds_info("MEMORY_BARRIER scope:%02x set_before=%02x set_after=%02x\n",
                         param->memoryBarrier.scope,
                         param->memoryBarrier.set_before,
                         param->memoryBarrier.set_after);
                break;
#endif
        default:
                gds_err("unsupported operation=%d\n", param->operation);
                break;
        }
}

//-----------------------------------------------------------------------------

void gds_dump_params(gds_op_list_t &params)
{
        for (unsigned int n = 0; n < params.size(); ++n) {
                CUstreamBatchMemOpParams *param = &params[n];
                gds_info("param[%d]:\n", n);
                gds_dump_param(param);
        }
}

//-----------------------------------------------------------------------------

int gds_fill_membar(gds_peer *peer, gds_op_list_t &ops, int flags)
{
        int retcode = 0;
#if HAVE_DECL_CU_STREAM_MEM_OP_MEMORY_BARRIER
        CUstreamBatchMemOpParams param;
        // TODO: sanity check flags
        if (flags & GDS_MEMBAR_FLUSH_REMOTE) {
                param.operation = CU_STREAM_MEM_OP_FLUSH_REMOTE_WRITES;
                param.flushRemoteWrites.flags = 0;
                gds_dbg("op=%d flush_remote flags=%08x\n",
                        param.operation,
                        param.flushRemoteWrites.flags);
        } else {
                param.operation = CU_STREAM_MEM_OP_MEMORY_BARRIER;
                if (flags & GDS_MEMBAR_MLX5) {
                        param.memoryBarrier.set_before = CU_STREAM_MEMORY_BARRIER_OP_WRITE_32 | CU_STREAM_MEMORY_BARRIER_OP_WRITE_64;
                } else {
                        param.memoryBarrier.set_before = CU_STREAM_MEMORY_BARRIER_OP_ALL;
                }
                param.memoryBarrier.set_after = CU_STREAM_MEMORY_BARRIER_OP_ALL;
                if (flags & GDS_MEMBAR_DEFAULT) {
                        param.memoryBarrier.scope = CU_STREAM_MEMORY_BARRIER_SCOPE_GPU;
                } else if (flags & GDS_MEMBAR_SYS) {
                        param.memoryBarrier.scope = CU_STREAM_MEMORY_BARRIER_SCOPE_SYS;
                } else {
                        gds_err("error, unsupported membar\n");
                        retcode = EINVAL;
                        goto out;
                }
                gds_dbg("op=%d membar scope:%02x set_before=%02x set_after=%02x\n",
                        param.operation,
                        param.memoryBarrier.scope,
                        param.memoryBarrier.set_before,
                        param.memoryBarrier.set_after);

        }
        ops.push_back(param);
out:
#else
        gds_err("CU_STREAM_MEM_OP_MEMORY_BARRIER not supported nor enabled on this GPU\n");
        retcode = EINVAL;
#endif
        return retcode;
}

//-----------------------------------------------------------------------------

static int gds_fill_inlcpy(gds_peer *peer, gds_op_list_t &ops, CUdeviceptr addr, const void *data, size_t n_bytes, int flags)
{
        int retcode = 0;
#if HAVE_DECL_CU_STREAM_MEM_OP_WRITE_MEMORY
        CUstreamBatchMemOpParams param;
        CUdeviceptr dev_ptr = addr;

        assert(addr);
        assert(n_bytes > 0);
        // TODO:
        //  verify address requirements of inline_copy

        // TODO: sanity check flags
        bool need_pre_barrier = (flags & GDS_WRITE_MEMORY_PRE_BARRIER_SYS) ? true : false;
        bool need_post_barrier = (flags & GDS_WRITE_MEMORY_POST_BARRIER_SYS) ? true : false;

        if (need_pre_barrier) {
                retcode = gds_fill_membar(peer, ops, GDS_MEMBAR_SYS);
                if (retcode)
                        return retcode;
        }

        param.operation = CU_STREAM_MEM_OP_WRITE_MEMORY;
        param.writeMemory.byteCount = n_bytes;
        param.writeMemory.src = const_cast<void *>(data);
        param.writeMemory.dst = dev_ptr;
        if (need_post_barrier)
                param.writeMemory.flags = CU_STREAM_WRITE_MEMORY_FENCE_SYS;
        else
                param.writeMemory.flags = CU_STREAM_WRITE_MEMORY_NO_MEMORY_BARRIER;
        gds_dbg("op=%d addr=%p src=%p size=%zd flags=%08x\n",
                param.operation,
                (void*)param.writeMemory.dst,
                param.writeMemory.src,
                param.writeMemory.byteCount,
                param.writeMemory.flags);
        ops.push_back(param);
#else
        gds_err("CU_STREAM_MEM_OP_WRITE_MEMORY not supported nor enabled on this GPU\n");
        retcode = EINVAL;
#endif
        return retcode;
}

int gds_fill_inlcpy(gds_peer *peer, gds_op_list_t &ops, void *ptr, const void *data, size_t n_bytes, int flags)
{
        int retcode = 0;
        CUdeviceptr dev_ptr = 0;
        retcode = gds_map_mem(ptr, n_bytes, memtype_from_flags(flags), &dev_ptr);
        if (retcode) {
                gds_err("could not lookup %p\n", ptr);
                goto out;
        }

        retcode = gds_fill_inlcpy(peer, ops, dev_ptr, data, n_bytes, flags);
out:
        return retcode;
}

//-----------------------------------------------------------------------------

static void gds_enable_barrier_for_inlcpy(CUstreamBatchMemOpParams *param)
{
#if HAVE_DECL_CU_STREAM_MEM_OP_WRITE_MEMORY
        assert(param->operation == CU_STREAM_MEM_OP_WRITE_MEMORY);
        param->writeMemory.flags &= ~CU_STREAM_WRITE_MEMORY_NO_MEMORY_BARRIER;
#endif
}

//-----------------------------------------------------------------------------

static int gds_fill_poke(gds_peer *peer, gds_op_list_t &ops, CUdeviceptr addr, uint32_t value, int flags)
{
        int retcode = 0;
        CUdeviceptr dev_ptr = addr;

        // TODO: convert into errors
        assert(addr);
        assert((((unsigned long)addr) & 0x3) == 0); 

        // TODO: sanity check flags
        bool need_barrier = (flags  & GDS_WRITE_PRE_BARRIER ) ? true : false;
        CUstreamBatchMemOpParams param;
        param.operation = CU_STREAM_MEM_OP_WRITE_VALUE_32;
        param.writeValue.address = dev_ptr;
        param.writeValue.value = value;
        param.writeValue.flags = CU_STREAM_WRITE_VALUE_NO_MEMORY_BARRIER;
        if (need_barrier)
                param.writeValue.flags = 0;
        gds_dbg("op=%d addr=%p value=%08x flags=%08x\n",
                param.operation,
                (void*)param.writeValue.address,
                param.writeValue.value,
                param.writeValue.flags);
        ops.push_back(param);
        return retcode;
}

int gds_fill_poke(gds_peer *peer, gds_op_list_t &ops, uint32_t *ptr, uint32_t value, int flags)
{
        int retcode = 0;
        CUdeviceptr dev_ptr = 0;

        gds_dbg("addr=%p value=%08x flags=%08x\n", ptr, value, flags);

        retcode = gds_map_mem(ptr, sizeof(*ptr), memtype_from_flags(flags), &dev_ptr);
        if (retcode) {
                gds_err("error %d while looking up %p\n", retcode, ptr);
                goto out;
        }

        retcode = gds_fill_poke(peer, ops, dev_ptr, value, flags);
out:
        return retcode;
}

//-----------------------------------------------------------------------------

static int gds_fill_poke64(gds_peer *peer, gds_op_list_t &ops, CUdeviceptr addr, uint64_t value, int flags)
{
        int retcode = 0;
#if HAVE_DECL_CU_STREAM_MEM_OP_WRITE_VALUE_64
        CUdeviceptr dev_ptr = addr;

        // TODO: convert into errors
        assert(addr);
        assert((((unsigned long)addr) & 0x7) == 0); 

        // TODO: sanity check flags
        bool need_barrier = (flags  & GDS_WRITE_PRE_BARRIER ) ? true : false;

        CUstreamBatchMemOpParams param;
        param.operation = CU_STREAM_MEM_OP_WRITE_VALUE_64;
        param.writeValue.address = dev_ptr;
        param.writeValue.value64 = value;
        param.writeValue.flags = CU_STREAM_WRITE_VALUE_NO_MEMORY_BARRIER;
        if (need_barrier)
                param.writeValue.flags = 0;
        gds_dbg("op=%d addr=%p value=%08x flags=%08x\n",
                param.operation,
                (void*)param.writeValue.address,
                param.writeValue.value,
                param.writeValue.flags);
        ops.push_back(param);
#else
        gds_err("CU_STREAM_WRITE_VALUE_NO_MEMORY_BARRIER not supported nor enabled on this GPU\n");
        retcode = EINVAL;
#endif
        return retcode;
}

int gds_fill_poke64(gds_peer *peer, gds_op_list_t &ops, uint64_t *ptr, uint64_t value, int flags)
{
        int retcode = 0;
        CUdeviceptr dev_ptr = 0;

        gds_dbg("addr=%p value=%016lx flags=%08x\n", ptr, value, flags);

        retcode = gds_map_mem(ptr, sizeof(*ptr), memtype_from_flags(flags), &dev_ptr);
        if (retcode) {
                gds_err("error %d while looking up %p\n", retcode, ptr);
                goto out;
        }

        retcode = gds_fill_poke64(peer, ops, dev_ptr, value, flags);
out:
        return retcode;
}

//-----------------------------------------------------------------------------

struct poll_checker {
        struct buf {
                uint64_t addr;
                uint32_t msk;
                uint32_t pad1;
                uint32_t state;
                uint32_t pad2;
        } *m_buf;
        unsigned m_idx;
        static unsigned m_global_index;

        poll_checker() {
                m_buf = (struct buf *)calloc(1, sizeof(*m_buf));
                assert(m_buf);
                m_idx = m_global_index++;
        }

        ~poll_checker() {
                free(m_buf);
        }

        void pre(gds_peer *peer, gds_op_list_t &ops, CUdeviceptr ptr, uint32_t magic, int cond_flag) {
                assert(m_buf);
                m_buf->addr = (uint64_t)ptr;
                m_buf->msk = magic;
                // verify ptr can be dereferenced on CPU
                uint64_t tmp = gds_atomic_get(reinterpret_cast<uint64_t*>(ptr));
                assert(cond_flag == GDS_WAIT_COND_NOR);
                gds_dbg("%d injecting pre poke\n", m_idx);
                gds_fill_poke(peer, ops, &m_buf->state, 1, GDS_MEMORY_HOST);
        }
        void post(gds_peer *peer, gds_op_list_t &ops) {
                gds_dbg("%d injecting post poke\n", m_idx);
                gds_fill_poke(peer, ops, &m_buf->state, 2, GDS_MEMORY_HOST);
        }

        bool run() {
                uint32_t *pw = reinterpret_cast<uint32_t*>(m_buf->addr);
                uint32_t value = gds_atomic_get(pw);
                uint32_t state = gds_atomic_get(&m_buf->state);
                uint32_t nor = ~(value | m_buf->msk);
                bool keep_running = true;
                if (state == 0) {
                        gds_dbg("%u NOR addr=%p value=%08x nor=%08x still not observed by GPU\n", m_idx, pw, value, nor);
                } else if (state == 1) {
                        gds_dbg("%u NOR addr=%p value=%08x nor=%08x is being observed by GPU\n", m_idx, pw, value, nor);
                } else if (state == 2) {
                        gds_dbg("%u NOR addr=%p value=%08x nor=%08x is all set, dequeing\n", m_idx, pw, value, nor);
                        if (nor) {
                                keep_running = false;
                                delete this;
                        }
                }
                return keep_running;
        }
};

unsigned poll_checker::m_global_index = 0;

//-----------------------------------------------------------------------------

static int gds_fill_poll(gds_peer *peer, gds_op_list_t &ops, CUdeviceptr ptr, uint32_t magic, int cond_flag, int flags)
{
        int retcode = 0;
        const char *cond_str = NULL;
        CUdeviceptr dev_ptr = ptr;
        poll_checker *ck = NULL;

        assert(ptr);
        assert((((unsigned long)ptr) & 0x3) == 0);

        // TODO: sanity check flags
        bool need_flush = (flags & GDS_WAIT_POST_FLUSH_REMOTE) ? true : false;
        if (!peer->has_remote_flush) {
                need_flush=false;
                gds_warn_once("RDMA consistency for pre-launched GPU work is not guaranteed at the moment\n");
        }

        CUstreamBatchMemOpParams param;
        param.operation = CU_STREAM_MEM_OP_WAIT_VALUE_32;
        param.waitValue.address = dev_ptr;
        param.waitValue.value = magic;
        switch(cond_flag) {
        case GDS_WAIT_COND_GEQ:
                param.waitValue.flags = CU_STREAM_WAIT_VALUE_GEQ;
                cond_str = "CU_STREAM_WAIT_VALUE_GEQ";
                break;
        case GDS_WAIT_COND_EQ:
                param.waitValue.flags = CU_STREAM_WAIT_VALUE_EQ;
                cond_str = "CU_STREAM_WAIT_VALUE_EQ";
                break;
        case GDS_WAIT_COND_AND:
                param.waitValue.flags = CU_STREAM_WAIT_VALUE_AND;
                cond_str = "CU_STREAM_WAIT_VALUE_AND";
                break;

        case GDS_WAIT_COND_NOR:
#if HAVE_DECL_CU_STREAM_WAIT_VALUE_NOR
                if (!peer->has_wait_nor) {
                        gds_err("GDS_WAIT_COND_NOR is not supported nor enabled on this GPU\n");
                        retcode = EINVAL;
                        goto out;
                }
                param.waitValue.flags = CU_STREAM_WAIT_VALUE_NOR;
                if (gds_enable_wait_checker())
                        ck = new poll_checker();
#else
                gds_err("GDS_WAIT_COND_NOR requires CUDA 9.0 at least\n");
                retcode = EINVAL;
#endif
                cond_str = "CU_STREAM_WAIT_VALUE_NOR";
                break;
        default: 
                gds_err("invalid wait condition flag\n");
                retcode = EINVAL;
                goto out;
        }

        if (need_flush)
                param.waitValue.flags |= CU_STREAM_WAIT_VALUE_FLUSH;

        gds_dbg("op=%d addr=%p value=%08x cond=%s flags=%08x\n",
                param.operation,
                (void*)param.waitValue.address,
                param.waitValue.value,
                cond_str,
                param.waitValue.flags);

        if (ck)
                ck->pre(peer, ops, ptr, magic, cond_flag);
        ops.push_back(param);
        if (ck) {
                ck->post(peer, ops);
                peer->tq->queue(std::bind(&poll_checker::run, ck));
        }
out:
        return retcode;
}

int gds_fill_poll(gds_peer *peer, gds_op_list_t &ops, uint32_t *ptr, uint32_t magic, int cond_flag, int flags)
{
        int retcode = 0;
        CUdeviceptr dev_ptr = 0;

        gds_dbg("addr=%p value=%08x cond=%08x flags=%08x\n", ptr, magic, cond_flag, flags);

        retcode = gds_map_mem(ptr, sizeof(*ptr), memtype_from_flags(flags), &dev_ptr);
        if (retcode) {
                gds_err("could not lookup %p\n", ptr);
                goto out;
        }
        
        retcode = gds_fill_poll(peer, ops, dev_ptr, magic, cond_flag, flags);
out:
        return retcode;
}

//-----------------------------------------------------------------------------

int gds_stream_batch_ops(gds_peer *peer, CUstream stream, gds_op_list_t &ops, int flags)
{
        CUresult result = CUDA_SUCCESS;
        int retcode = 0;
        unsigned int cuflags = 0;
        size_t nops = ops.size();

        if (gds_enable_weak_consistency() && peer->has_weak)
                cuflags |= CU_STREAM_BATCH_MEM_OP_RELAXED_ORDERING;

        gds_dbg("nops=%zu flags=%08x\n", nops, cuflags);

        if (nops > peer->max_batch_size) {
                gds_warn("batch size might be too big, stream=%p nops=%zu flags=%08x\n", stream, nops, flags);
                //return EINVAL;
        }

        result = cuStreamBatchMemOp(stream, nops, &ops[0], cuflags);
        if (CUDA_SUCCESS != result) {
                const char *err_str = NULL;
                cuGetErrorString(result, &err_str);
                gds_err("got CUDA result %d (%s) while submitting batch operations:\n", result, err_str);
                retcode = gds_curesult_to_errno(result);
                gds_err("retcode=%d nops=%zu flags=%08x, dumping memops:\n", retcode, nops, cuflags);
                gds_dump_params(ops);
                goto out;
        }

        if (gds_enable_dump_memops()) {
                gds_info("nops=%zu flags=%08x\n", nops, cuflags);
                gds_dump_params(ops);
        }

out:        
        return retcode;
}

//-----------------------------------------------------------------------------

/*
  A) plain+membar:
  WR32
  MEMBAR
  WR32
  WR32

  B) plain:
  WR32
  WR32+PREBARRIER
  WR32

  C) sim64+membar:
  WR32
  MEMBAR
  INLCPY 8B

  D) sim64:
  INLCPY 4B + POSTBARRIER
  INLCPY 8B

  E) inlcpy+membar:
  WR32
  MEMBAR
  INLCPY XB

  F) inlcpy:
  INLCPY 4B + POSTBARRIER
  INLCPY 128B
*/

int gds_post_ops(gds_peer *peer, size_t n_ops, struct peer_op_wr *op, gds_op_list_t &ops, int post_flags)
{
        int retcode = 0;
        size_t n = 0;
        bool prev_was_fence = false;
        bool use_inlcpy_for_dword = false;
        //size_t n_ops = ops.size();
        CUstreamBatchMemOpParams param;

        gds_dbg("n_ops=%zu\n", n_ops);

        if (!peer->has_memops) {
                gds_err("CUDA MemOps are required\n");
                return EINVAL;
        }

        // divert the request to the same engine handling 64bits
        // to avoid out-of-order execution
        // caveat: can't use membar if inlcpy is used for 4B writes (to simulate 8B writes)
        if (peer->has_inlcpy) {
                if (!peer->has_membar)
                        use_inlcpy_for_dword = true; // F
        }
        if (gds_simulate_write64()) {
                if (!peer->has_membar) {
                        gds_warn_once("enabling use_inlcpy_for_dword\n");
                        use_inlcpy_for_dword = true; // D
                }
        }

        for (; op && n < n_ops; op = op->next, ++n) {
                //int flags = 0;
                gds_dbg("op[%zu] type:%08x\n", n, op->type);
                switch(op->type) {
                case IBV_EXP_PEER_OP_FENCE: {
                        gds_dbg("OP_FENCE: fence_flags=%" PRIu64 "\n", op->wr.fence.fence_flags);
                        uint32_t fence_op = (op->wr.fence.fence_flags & (IBV_EXP_PEER_FENCE_OP_READ|IBV_EXP_PEER_FENCE_OP_WRITE));
                        uint32_t fence_from = (op->wr.fence.fence_flags & (IBV_EXP_PEER_FENCE_FROM_CPU|IBV_EXP_PEER_FENCE_FROM_HCA));
                        uint32_t fence_mem = (op->wr.fence.fence_flags & (IBV_EXP_PEER_FENCE_MEM_SYS|IBV_EXP_PEER_FENCE_MEM_PEER));

                        if (fence_op == IBV_EXP_PEER_FENCE_OP_READ) {
                                gds_dbg("nothing to do for read fences\n");
                                //retcode = EINVAL;
                                break;
                        }
                        else {
                                if (!peer->has_membar) {
                                        if (use_inlcpy_for_dword) {
                                                assert(ops.size() > 0);
                                                gds_dbg("patching previous param\n");
                                                gds_enable_barrier_for_inlcpy(&ops.back());
                                        }
                                        else {
                                                gds_dbg("recording fence event\n");
                                                prev_was_fence = true;
                                        }
                                        //retcode = 0;
                                }
                                else {
                                        if (fence_from != IBV_EXP_PEER_FENCE_FROM_HCA) {
                                                gds_err("unexpected from fence\n");
                                                retcode = EINVAL;
                                                break;
                                        }
                                        int flags = 0;
                                        if (fence_mem == IBV_EXP_PEER_FENCE_MEM_PEER) {
                                                gds_dbg("using light membar\n");
                                                flags = GDS_MEMBAR_DEFAULT | GDS_MEMBAR_MLX5;
                                        }
                                        else if (fence_mem == IBV_EXP_PEER_FENCE_MEM_SYS) {
                                                gds_dbg("using heavy membar\n");
                                                flags = GDS_MEMBAR_SYS | GDS_MEMBAR_MLX5;
                                        }
                                        else {
                                                gds_err("unsupported fence combination\n");
                                                retcode = EINVAL;
                                                break;
                                        }
                                        retcode = gds_fill_membar(peer, ops, flags);
                                }
                        }
                        break;
                }
                case IBV_EXP_PEER_OP_STORE_DWORD: {
                        CUdeviceptr dev_ptr = range_from_id(op->wr.dword_va.target_id)->dptr + 
                                op->wr.dword_va.offset;
                        uint32_t data = op->wr.dword_va.data;
                        int flags = 0;
                        gds_dbg("OP_STORE_DWORD dev_ptr=%llx data=%" PRIx32 "\n", dev_ptr, data);
                        if (use_inlcpy_for_dword) { // F || D
                                // membar may be out of order WRT inlcpy
                                if (peer->has_membar) {
                                        gds_err("invalid feature combination, inlcpy + membar\n");
                                        retcode = EINVAL;
                                        break;
                                }
                                // tail flush is set when following fence is met
                                //  flags |= GDS_IMMCOPY_POST_TAIL_FLUSH;
                                retcode = gds_fill_inlcpy(peer, ops, dev_ptr, &data, sizeof(data), flags);
                        }
                        else {  // A || B || C || E
                                // can't guarantee ordering of write32+inlcpy unless
                                // a membar is there
                                // TODO: fix driver when !weak
                                if (peer->has_inlcpy && !peer->has_membar) {
                                        gds_err("invalid feature combination, inlcpy needs membar\n");
                                        retcode = EINVAL;
                                        break;
                                }
                                if (prev_was_fence) {
                                        gds_dbg("using PRE_BARRIER as fence\n");
                                        flags |= GDS_WRITE_PRE_BARRIER;
                                        prev_was_fence = false;
                                }
                                retcode = gds_fill_poke(peer, ops, dev_ptr, data, flags);
                        }
                        break;
                }
                case IBV_EXP_PEER_OP_STORE_QWORD: {
                        CUdeviceptr dev_ptr = range_from_id(op->wr.qword_va.target_id)->dptr +
                                op->wr.qword_va.offset;
                        uint64_t data = op->wr.qword_va.data;
                        int flags = 0;
                        gds_dbg("OP_STORE_QWORD dev_ptr=%llx data=%" PRIx64 "\n", dev_ptr, data);
                        // C || D
                        if (gds_simulate_write64()) {
                                // simulate 64-bit poke by inline copy
                                if (!peer->has_membar) {
                                        gds_err("invalid feature combination, inlcpy needs membar\n");
                                        retcode = EINVAL;
                                        break;
                                }

                                // tail flush is never useful here
                                //flags |= GDS_IMMCOPY_POST_TAIL_FLUSH;
                                retcode = gds_fill_inlcpy(peer, ops, dev_ptr, &data, sizeof(data), flags);
                        }
                        else if (peer->has_write64) {
                                retcode = gds_fill_poke64(peer, ops, dev_ptr, data, flags);
                        }
                        else {
                                uint32_t datalo = gds_qword_lo(op->wr.qword_va.data);
                                uint32_t datahi = gds_qword_hi(op->wr.qword_va.data);

                                if (prev_was_fence) {
                                        gds_dbg("enabling PRE_BARRIER\n");
                                        flags |= GDS_WRITE_PRE_BARRIER;
                                        prev_was_fence = false;
                                }
                                retcode = gds_fill_poke(peer, ops, dev_ptr, datalo, flags);

                                // get rid of the barrier, if there
                                flags &= ~GDS_WRITE_PRE_BARRIER;

                                // advance to next DWORD
                                dev_ptr += sizeof(uint32_t);
                                retcode = gds_fill_poke(peer, ops, dev_ptr, datahi, flags);
                        }

                        break;
                }
                case IBV_EXP_PEER_OP_COPY_BLOCK: {
                        CUdeviceptr dev_ptr = range_from_id(op->wr.copy_op.target_id)->dptr +
                                op->wr.copy_op.offset;
                        size_t len = op->wr.copy_op.len;
                        void *src = op->wr.copy_op.src;
                        int flags = 0;
                        gds_dbg("OP_COPY_BLOCK dev_ptr=%llx src=%p len=%zu\n", dev_ptr, src, len);
                        // catching any other size here
                        if (!peer->has_inlcpy) {
                                gds_err("inline copy is not supported\n");
                                retcode = EINVAL;
                                break;
                        }
                        // IB Verbs bug
                        assert(len <= GDS_GPU_MAX_INLINE_SIZE);
                        //if (desc->need_flush) {
                        //        flags |= GDS_IMMCOPY_POST_TAIL_FLUSH;
                        //}
                        retcode = gds_fill_inlcpy(peer, ops, dev_ptr, src, len, flags);
                        break;
                }
                case IBV_EXP_PEER_OP_POLL_AND_DWORD:
                case IBV_EXP_PEER_OP_POLL_GEQ_DWORD:
                case IBV_EXP_PEER_OP_POLL_NOR_DWORD: {
                        int poll_cond;
                        CUdeviceptr dev_ptr = range_from_id(op->wr.dword_va.target_id)->dptr + 
                                op->wr.dword_va.offset;
                        uint32_t data = op->wr.dword_va.data;
                        // TODO: properly handle a following fence instead of blidly flushing
                        int flags = 0;
                        if (!(post_flags & GDS_POST_OPS_DISCARD_WAIT_FLUSH))
                                flags |= GDS_WAIT_POST_FLUSH_REMOTE;

                        gds_dbg("OP_WAIT_DWORD dev_ptr=%llx data=%" PRIx32 " type=%" PRIx32 "\n", dev_ptr, data, (uint32_t)op->type);

                        switch(op->type) {
                        case IBV_EXP_PEER_OP_POLL_NOR_DWORD:
                                poll_cond = GDS_WAIT_COND_NOR;
                                break;
                        case IBV_EXP_PEER_OP_POLL_GEQ_DWORD:
                                poll_cond = GDS_WAIT_COND_GEQ;
                                break;
                        case IBV_EXP_PEER_OP_POLL_AND_DWORD:
                                poll_cond = GDS_WAIT_COND_AND;
                                break;
                        default:
                                assert(!"cannot happen");
                                retcode = EINVAL;
                                goto out;
                        }
                        retcode = gds_fill_poll(peer, ops, dev_ptr, data, poll_cond, flags);
                        break;
                }
                default:
                        gds_err("undefined peer op type %d\n", op->type);
                        retcode = EINVAL;
                        break;
                }
                if (retcode) {
                        gds_err("error in fill func at entry n=%zu\n", n);
                        goto out;
                }
        }

        assert(n_ops == n);

out:
        return retcode;
}

//-----------------------------------------------------------------------------

int gds_post_pokes(CUstream stream, int count, gds_send_request_t *info, uint32_t *dw, uint32_t val)
{
        int retcode = 0;
        //CUstreamBatchMemOpParams params[poke_count+1];
        gds_op_list_t ops;

        assert(info);
        assert(dw);

        gds_peer *peer = peer_from_stream(stream);
        if (!peer) {
                return EINVAL;
        }

        for (int j=0; j<count; j++) {
                gds_dbg("peer_commit:%d\n", j);
                retcode = gds_post_ops(peer, info[j].commit.entries, info[j].commit.storage, ops, 0);
                if (retcode) {
                        goto out;
                }
        }

        if (dw) {
                // assume host mem
                retcode = gds_fill_poke(peer, ops, dw, val, GDS_MEMORY_HOST);
                if (retcode) {
                        gds_err("error %d at tracking entry\n", retcode);
                        goto out;
                }
        }

        retcode = gds_stream_batch_ops(peer, stream, ops, 0);
        if (retcode) {
                gds_err("error %d in stream_batch_ops\n", retcode);
                goto out;
        }
out:

        return retcode;
}

//-----------------------------------------------------------------------------

int gds_post_ops_on_cpu(size_t n_ops, struct peer_op_wr *op, int post_flags)
{
        int retcode = 0;
        size_t n = 0;
        gds_dbg("n_ops=%zu op=%p post_flags=0x%x\n", n_ops, op, post_flags);
        for (; op && n < n_ops; op = op->next, ++n) {
                //int flags = 0;
                gds_dbg("op[%zu]=%p\n", n, op);
                //gds_dbg("op[%zu]=%p type:%08x\n", n, op, op->type);
                switch(op->type) {
                case IBV_EXP_PEER_OP_FENCE: {
                        gds_dbg("FENCE flags=%" PRIu64 "\n", op->wr.fence.fence_flags);
                        uint32_t fence_op = (op->wr.fence.fence_flags & (IBV_EXP_PEER_FENCE_OP_READ|IBV_EXP_PEER_FENCE_OP_WRITE));
                        uint32_t fence_from = (op->wr.fence.fence_flags & (IBV_EXP_PEER_FENCE_FROM_CPU|IBV_EXP_PEER_FENCE_FROM_HCA));
                        uint32_t fence_mem = (op->wr.fence.fence_flags & (IBV_EXP_PEER_FENCE_MEM_SYS|IBV_EXP_PEER_FENCE_MEM_PEER));

                        if (fence_op == IBV_EXP_PEER_FENCE_OP_READ) {
                                gds_warnc(1, "nothing to do for read fences\n");
                                //retcode = EINVAL;
                                break;
                        }
                        else {
                                if (fence_from != IBV_EXP_PEER_FENCE_FROM_HCA) {
                                        gds_err("unexpected from %08x fence, expected FROM_HCA\n", fence_from);
                                        retcode = EINVAL;
                                        break;
                                }
                                if (fence_mem == IBV_EXP_PEER_FENCE_MEM_PEER) {
                                        gds_dbg("using light membar\n");
                                        wmb();
                                }
                                else if (fence_mem == IBV_EXP_PEER_FENCE_MEM_SYS) {
                                        gds_dbg("using heavy membar\n");
                                        wmb();
                                }
                                else {
                                        gds_err("unsupported fence combination\n");
                                        retcode = EINVAL;
                                        break;
                                }
                        }
                        break;
                }
                case IBV_EXP_PEER_OP_STORE_DWORD: {
                        uint32_t *ptr = (uint32_t*)((ptrdiff_t)range_from_id(op->wr.dword_va.target_id)->va + op->wr.dword_va.offset);
                        uint32_t data = op->wr.dword_va.data;
                        // A || B || C || E
                        gds_dbg("STORE_DWORD ptr=%p data=%08" PRIx32 "\n", ptr, data);
                        gds_atomic_set(ptr, data);
                        break;
                }
                case IBV_EXP_PEER_OP_STORE_QWORD: {
                        uint64_t *ptr = (uint64_t*)((ptrdiff_t)range_from_id(op->wr.qword_va.target_id)->va + op->wr.qword_va.offset);
                        uint64_t data = op->wr.qword_va.data;
                        gds_dbg("STORE_QWORD ptr=%p data=%016" PRIx64 "\n", ptr, data);
                        gds_atomic_set(ptr, data);
                        break;
                }
                case IBV_EXP_PEER_OP_COPY_BLOCK: {
                        uint64_t *ptr = (uint64_t*)((ptrdiff_t)range_from_id(op->wr.copy_op.target_id)->va + op->wr.copy_op.offset);
                        uint64_t *src = (uint64_t*)op->wr.copy_op.src;
                        size_t n_bytes = op->wr.copy_op.len;
                        gds_dbg("COPY_BLOCK ptr=%p src=%p len=%zu\n", ptr, src, n_bytes);
                        gds_bf_copy(ptr, src, n_bytes);
                        break;
                }
                case IBV_EXP_PEER_OP_POLL_AND_DWORD:
                case IBV_EXP_PEER_OP_POLL_GEQ_DWORD:
                case IBV_EXP_PEER_OP_POLL_NOR_DWORD: {
                        int poll_cond;
                        uint32_t *ptr = (uint32_t*)((ptrdiff_t)range_from_id(op->wr.dword_va.target_id)->va + op->wr.dword_va.offset);
                        uint32_t value = op->wr.dword_va.data;
                        bool flush = true;
                        if (post_flags & GDS_POST_OPS_DISCARD_WAIT_FLUSH)
                                flush = false;
                        gds_dbg("WAIT_32 dev_ptr=%p data=%" PRIx32 " type=%" PRIx32 "\n", ptr, value, (uint32_t)op->type);
                        bool done = false;
                        do {
                                uint32_t data = gds_atomic_get(ptr);
                                switch(op->type) {
                                case IBV_EXP_PEER_OP_POLL_NOR_DWORD:
                                        done = ~(data | value);
                                        break;
                                case IBV_EXP_PEER_OP_POLL_GEQ_DWORD:
                                        done = ((int32_t)data - (int32_t)value >= 0);
                                        break;
                                case IBV_EXP_PEER_OP_POLL_AND_DWORD:
                                        done = (data & value);
                                        break;
                                default:
                                        gds_err("invalid op type %02x\n", op->type);
                                        retcode = EINVAL;
                                        goto out;
                                }
                                if (done)
                                        break;
                                // TODO: more aggressive CPU relaxing needed here to avoid starving I/O fabric
                                arch_cpu_relax();
                        } while(true);
                        break;
                }
                default:
                        gds_err("undefined peer op type %d\n", op->type);
                        retcode = EINVAL;
                        break;
                }
                if (retcode) {
                        gds_err("error %d at entry n=%zu\n", retcode, n);
                        goto out;
                }
        }

out:
        return retcode;
}

//-----------------------------------------------------------------------------

int gds_post_pokes_on_cpu(int count, gds_send_request_t *info, uint32_t *dw, uint32_t val)
{
        int retcode = 0;
        int idx = 0;

        assert(info);

        for (int j=0; j<count; j++) {
                gds_dbg("peer_commit:%d idx=%d\n", j, idx);
                retcode = gds_post_ops_on_cpu(info[j].commit.entries, info[j].commit.storage);
                if (retcode) {
                        goto out;
                }
        }

        if (dw) {
                wmb();
                gds_atomic_set(dw, val);
        }

out:
        return retcode;
}

//-----------------------------------------------------------------------------

static void gds_dump_ops(struct peer_op_wr *op, size_t count)
{
        size_t n = 0;
        for (; op; op = op->next, ++n) {
                gds_dbg("op[%zu] type:%d\n", n, op->type);
                switch(op->type) {
                case IBV_EXP_PEER_OP_FENCE: {
                        gds_dbg("FENCE flags=%" PRIu64 "\n", op->wr.fence.fence_flags);
                        break;
                }
                case IBV_EXP_PEER_OP_STORE_DWORD: {
                        CUdeviceptr dev_ptr = range_from_id(op->wr.dword_va.target_id)->dptr + 
                                op->wr.dword_va.offset;
                        gds_dbg("STORE_QWORD data:%x target_id:%" PRIx64 " offset:%zu dev_ptr=%llx\n",
                                op->wr.dword_va.data, op->wr.dword_va.target_id,
                                op->wr.dword_va.offset, dev_ptr);
                        break;
                }
                case IBV_EXP_PEER_OP_STORE_QWORD: {
                        CUdeviceptr dev_ptr = range_from_id(op->wr.qword_va.target_id)->dptr +
                                op->wr.qword_va.offset;
                        gds_dbg("STORE_QWORD data:%" PRIx64 " target_id:%" PRIx64 " offset:%zu dev_ptr=%llx\n",
                                op->wr.qword_va.data, op->wr.qword_va.target_id,
                                op->wr.qword_va.offset, dev_ptr);
                        break;
                }
                case IBV_EXP_PEER_OP_COPY_BLOCK: {
                        CUdeviceptr dev_ptr = range_from_id(op->wr.copy_op.target_id)->dptr +
                                op->wr.copy_op.offset;
                        gds_dbg("COPY_BLOCK src:%p len:%zu target_id:%" PRIx64 " offset:%zu dev_ptr=%llx\n",
                                op->wr.copy_op.src, op->wr.copy_op.len,
                                op->wr.copy_op.target_id, op->wr.copy_op.offset,
                                dev_ptr);
                        break;
                }
                case IBV_EXP_PEER_OP_POLL_AND_DWORD:
                case IBV_EXP_PEER_OP_POLL_NOR_DWORD: {
                        CUdeviceptr dev_ptr = range_from_id(op->wr.dword_va.target_id)->dptr + 
                                op->wr.dword_va.offset;
                        gds_dbg("%s data:%08x target_id:%" PRIx64 " offset:%zu dev_ptr=%llx\n", 
                                (op->type==IBV_EXP_PEER_OP_POLL_AND_DWORD) ? "POLL_AND_DW" : "POLL_NOR_SDW",
                                op->wr.dword_va.data, 
                                op->wr.dword_va.target_id, 
                                op->wr.dword_va.offset, 
                                dev_ptr);
                        break;
                }
                default:
                        gds_err("undefined peer op type %d\n", op->type);
                        break;
                }
        }

        assert(count == n);
}

//-----------------------------------------------------------------------------

void gds_dump_wait_request(gds_wait_request_t *request, size_t count)
{
        for (size_t j=0; j<count; ++j) {
                struct ibv_exp_peer_peek *peek = &request[j].peek;
                gds_dbg("req[%zu] entries:%u whence:%u offset:%u peek_id:%" PRIx64 " comp_mask:%08x\n", 
                        j, peek->entries, peek->whence, peek->offset, 
                        peek->peek_id, peek->comp_mask);
                gds_dump_ops(peek->storage, peek->entries);
        }
}

//-----------------------------------------------------------------------------

int gds_stream_post_wait_cq_multi(CUstream stream, int count, gds_wait_request_t *request, uint32_t *dw, uint32_t val)
{
        int retcode = 0;
        int n_mem_ops = 0;
        int idx = 0;
        int k=0;
        gds_descriptor_t * descs=NULL;

        assert(request);
        assert(count);

        descs = (gds_descriptor_t *) calloc(count, sizeof(gds_descriptor_t));
        if(!descs)
        {
                gds_err("Calloc for %d elements\n", count);
                retcode=ENOMEM;
                goto out;
        }

        for (k=0; k<count; k++) {
                descs[k].tag = GDS_TAG_WAIT;
                descs[k].wait = &request[k];
        }

        retcode=gds_stream_post_descriptors(stream, count, descs, 0);
        if (retcode) {
                gds_err("error %d in gds_stream_post_descriptors\n", retcode);
                goto out;
        }

        out:
                if(descs) free(descs);
                return retcode;
}

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------

// If NULL returned then buffer will be allocated in system memory
// by ibverbs driver.
static struct ibv_exp_peer_buf *gds_buf_alloc(ibv_exp_peer_buf_alloc_attr *attr)
{
        assert(attr);
        gds_peer *peer = peer_from_id(attr->peer_id);
        assert(peer);

        gds_dbg("alloc mem peer:{type=%d gpu_id=%d} attr{len=%zu dir=%d alignment=%d peer_id=%" PRIx64 "}\n",
                peer->alloc_type, peer->gpu_id, attr->length, attr->dir, attr->alignment, attr->peer_id);

        return peer->buf_alloc(peer->alloc_type, attr->length, attr->dir, attr->alignment, peer->alloc_flags);
}

static int gds_buf_release(struct ibv_exp_peer_buf *pb)
{
        gds_dbg("freeing pb=%p\n", pb);
        gds_buf *buf = static_cast<gds_buf*>(pb);
        gds_peer *peer = buf->peer;
        peer->free(buf);
        return 0;
}

static uint64_t gds_register_va(void *start, size_t length, uint64_t peer_id, struct ibv_exp_peer_buf *pb)
{
        gds_peer *peer = peer_from_id(peer_id);
        gds_range *range = NULL;

        gds_dbg("start=%p length=%zu peer_id=%" PRIx64 " peer_buf=%p\n", start, length, peer_id, pb);

        if (IBV_EXP_PEER_IOMEMORY == pb) {
                // register as IOMEM
                range = peer->register_range(start, length, GDS_MEMORY_IO);
        }
        else if (pb) {
                gds_buf *buf = static_cast<gds_buf*>(pb);
                // should have been allocated via gds_buf_alloc
                // assume GDR mapping already created
                // associate range to peer_buf
                range = peer->range_from_buf(buf, start, length);
        }
        else {
                // register as SYSMEM
                range = peer->register_range(start, length, GDS_MEMORY_HOST);
        }
        if (!range) {
                gds_err("error while registering range, returning 0 as error value\n");
                return 0;
        }
        return range_to_id(range);
}

static int gds_unregister_va(uint64_t registration_id, uint64_t peer_id)
{
        gds_peer *peer = peer_from_id(peer_id);
        gds_range *range = range_from_id(registration_id);
        gds_dbg("peer=%p range=%p\n", peer, range);
        peer->unregister(range);
        return 0;
}

static bool support_memops(CUdevice dev)
{
        int flag = 0;
#if HAVE_DECL_CU_DEVICE_ATTRIBUTE_CAN_USE_STREAM_MEM_OPS
        // on  CUDA_VERSION >= 9010
        CUCHECK(cuDeviceGetAttribute(&flag, CU_DEVICE_ATTRIBUTE_CAN_USE_STREAM_MEM_OPS, dev));
#elif CUDA_VERSION >= 8000
        // CUDA MemOps are always enabled on CUDA 8.0+
        flag = 1;
#else
#error "CUDA MemOp APIs are missing prior to CUDA 8.0"
#endif
        gds_dbg("dev=%d has_memops=%d\n", dev, flag);
        return !!flag;
}

static bool support_remote_flush(CUdevice dev)
{
        int flag = 0;
#if HAVE_DECL_CU_DEVICE_ATTRIBUTE_CAN_FLUSH_REMOTE_WRITES
        // on CUDA_VERSION >= 9020
        CUCHECK(cuDeviceGetAttribute(&flag, CU_DEVICE_ATTRIBUTE_CAN_FLUSH_REMOTE_WRITES, dev));
#else
#warning "Assuming CU_DEVICE_ATTRIBUTE_CAN_FLUSH_REMOTE_WRITES=0 prior to CUDA 9.2"
#endif
        gds_dbg("dev=%d has_remote_flush=%d\n", dev, flag);
        return !!flag;
}

static bool support_write64(CUdevice dev)
{
        int flag = 0;
#if HAVE_DECL_CU_DEVICE_ATTRIBUTE_CAN_USE_64_BIT_STREAM_MEM_OPS
        // on CUDA_VERSION >= 9000
        CUCHECK(cuDeviceGetAttribute(&flag, CU_DEVICE_ATTRIBUTE_CAN_USE_64_BIT_STREAM_MEM_OPS, dev));
#endif
        gds_dbg("dev=%d has_write64=%d\n", dev, flag);
        return !!flag;
}

static bool support_wait_nor(CUdevice dev)
{
        int flag = 0;
#if HAVE_DECL_CU_DEVICE_ATTRIBUTE_CAN_USE_STREAM_WAIT_VALUE_NOR
        // on CUDA_VERSION >= 9000
        CUCHECK(cuDeviceGetAttribute(&flag, CU_DEVICE_ATTRIBUTE_CAN_USE_STREAM_WAIT_VALUE_NOR, dev));
#else
        gds_dbg("hardcoding has_wait_nor=0\n");
#endif
        gds_dbg("dev=%d has_wait_nor=%d\n", dev, flag);
        return !!flag;
}

static bool support_inlcpy(CUdevice dev)
{
        int flag = 0;
#if HAVE_DECL_CU_DEVICE_ATTRIBUTE_CAN_USE_STREAM_WRITE_MEMORY
        CUresult rc = cuDeviceGetAttribute(&flag, CU_DEVICE_ATTRIBUTE_CAN_USE_STREAM_WRITE_MEMORY, dev);
        if (rc == CUDA_ERROR_INVALID_VALUE) {
                // expected if driver not matching the cuda.h
                flag = 0;
        } else if (rc) {
                // any other error message is not expected
                CUCHECK(rc);
        }
#else
        gds_dbg("hardcoding has_inlcpy=0\n");
#endif
        gds_dbg("dev=%d has_inlcpy=%d\n", dev, flag);
        return !!flag;
}

static bool support_membar(CUdevice dev)
{
        int flag = 0;
#if HAVE_DECL_CU_DEVICE_ATTRIBUTE_CAN_USE_STREAM_MEMORY_BARRIER
        CUresult rc = cuDeviceGetAttribute(&flag, CU_DEVICE_ATTRIBUTE_CAN_USE_STREAM_MEMORY_BARRIER, dev);
        if (rc == CUDA_ERROR_INVALID_VALUE) {
                // expected if driver not matching the cuda.h
                flag = 0;
        } else if (rc) {
                // any other error message is not expected
                CUCHECK(rc);
        }
#else
        gds_dbg("hardcoding has_membar=0\n");
#endif
        gds_dbg("dev=%d has_membar=%d\n", dev, flag);
        return !!flag;
}

static bool support_weak_consistency(CUdevice dev)
{
        int flag = 0;
        CUdevice cur_dev;
        bool has_hidden_flag = false;

#if HAVE_DECL_CU_DEVICE_ATTRIBUTE_CAN_USE_STREAM_BATCH_MEMOP_RELAXED_ORDERING
        CUresult rc = cuDeviceGetAttribute(&flag, CU_DEVICE_ATTRIBUTE_CAN_USE_STREAM_BATCH_MEMOP_RELAXED_ORDERING, dev);
        if (rc == CUDA_ERROR_INVALID_VALUE) {
                // expected if driver not matching the cuda.h
                flag = 0;
        } else if (rc) {
                // any other error message is not expected
                CUCHECK(rc);
        }
#endif

        CUCHECK(cuCtxGetDevice(&cur_dev));
        if (cur_dev != dev) {
                gds_err("device context is not current, cannot detect weak consistency flag\n");
                goto done;
        }

        do {
                gds_dbg("testing hidden weak flag\n");
                        
                CUstreamBatchMemOpParams params[2];
                CUresult res;
                res = cuStreamBatchMemOp(0, 0, params, 0);
                if (res == CUDA_ERROR_NOT_SUPPORTED) {
                        gds_err("Either cuStreamBatchMemOp API is not supported on this platform or it has not been enabled, check libgdsync system requirements.\n");
                        break;
                } else if (res != CUDA_SUCCESS) {
                        const char *err_str = NULL;
                        cuGetErrorString(res, &err_str);
                        const char *err_name = NULL;
                        cuGetErrorName(res, &err_name);
                        gds_err("very serious problems with cuStreamBatchMemOp() %d(%s) '%s'\n", res, err_name, err_str);
                        break;
                }
                res = cuStreamBatchMemOp(0, 0, params, CU_STREAM_BATCH_MEM_OP_RELAXED_ORDERING);
                if (res == CUDA_ERROR_INVALID_VALUE) {
                        gds_dbg("weak flag is not supported\n");
                        break;
                } else if (res != CUDA_SUCCESS) {
                        const char *err_str = NULL;
                        cuGetErrorString(res, &err_str);
                        const char *err_name = NULL;
                        cuGetErrorName(res, &err_name);
                        gds_err("serious problems with cuStreamBatchMemOp() %d(%s) '%s'\n", res, err_name, err_str);
                        break;
                }
                gds_dbg("detected hidden weak consistency flag\n");
                has_hidden_flag = true;
        } while(0);

        if (flag && !has_hidden_flag) {
                gds_err("GPU dev=%d relaxed ordering device attribute and detection do not agree\n", dev);
                abort();
        }
done:                
        gds_dbg("dev=%d has_weak=%d\n", dev, has_hidden_flag);
        return has_hidden_flag;
}

//-----------------------------------------------------------------------------

static gds_peer gpu_peer[max_gpus];
static gds_peer_attr gpu_peer_attr[max_gpus];
static bool gpu_registered[max_gpus];

//-----------------------------------------------------------------------------

static void gds_init_peer(gds_peer *peer, CUdevice dev, CUcontext ctx, int gpu_id)
{
        assert(peer);

        peer->gpu_id = gpu_id;
        peer->gpu_dev = dev;
        peer->gpu_ctx = ctx;
        peer->has_memops = support_memops(dev);
        peer->has_remote_flush = support_remote_flush(dev) && gds_enable_remote_flush();
        peer->has_write64 = support_write64(dev) && gds_enable_write64();
        peer->has_wait_nor = support_wait_nor(dev) && gds_enable_wait_nor();
        peer->has_inlcpy = support_inlcpy(dev) && gds_enable_inlcpy();
        peer->has_membar = support_membar(dev);
        peer->has_weak = support_weak_consistency(dev);

        peer->max_batch_size = 256;

        peer->alloc_type = gds_peer::NONE;
        peer->alloc_flags = 0;

        peer->attr.peer_id = peer_to_id(peer);
        peer->attr.buf_alloc = gds_buf_alloc;
        peer->attr.buf_release = gds_buf_release;
        peer->attr.register_va = gds_register_va;
        peer->attr.unregister_va = gds_unregister_va;

        peer->attr.caps = ( IBV_EXP_PEER_OP_STORE_DWORD_CAP    | 
                            IBV_EXP_PEER_OP_STORE_QWORD_CAP    | 
                            IBV_EXP_PEER_OP_FENCE_CAP          | 
                            IBV_EXP_PEER_OP_POLL_AND_DWORD_CAP );

        if (peer->has_wait_nor) {
                gds_dbg("enabling NOR feature\n");
                peer->attr.caps |= IBV_EXP_PEER_OP_POLL_NOR_DWORD_CAP;
        } else
                peer->attr.caps |= IBV_EXP_PEER_OP_POLL_GEQ_DWORD_CAP;

        if (peer->has_inlcpy) {
                gds_dbg("enabling COPY BLOCK feature\n");
                peer->attr.caps |= IBV_EXP_PEER_OP_COPY_BLOCK_CAP;
        }
        else if (peer->has_write64 || gds_simulate_write64()) {
                gds_dbg("enabling STORE QWORD feature\n");
                peer->attr.caps |= IBV_EXP_PEER_OP_STORE_QWORD_CAP;
        }
        gds_dbg("caps=%016lx\n", peer->attr.caps);
        peer->attr.peer_dma_op_map_len = GDS_GPU_MAX_INLINE_SIZE;
        peer->attr.comp_mask = IBV_EXP_PEER_DIRECT_VERSION;
        peer->attr.version = 1;

        peer->tq = new task_queue;

        CUcontext cur_ctx;
        CUCHECK(cuCtxGetCurrent(&cur_ctx));
        GDS_ASSERT(cur_ctx == ctx);
#if 0
        // need to load kernel cubin, but driver API requires a different approach
        //CUCHECK(cuModuleLoadData(&peer->opt_kernels_module, gds_module_file));
        //CUCHECK(cuModuleGetFunction(&peer->kernel_1qp_send_2cq_wait, peer->opt_kernels_module, "gds_1QPSend_2CQWait"));

        // in the mean time, CUDA RT cudaLaunchKernel might be easier to use, as it
        // does not require additional setup procedures from the user
#endif

        int major;
        CUCHECK( cuDeviceGetAttribute(&major, CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MAJOR, peer->gpu_dev) );
        int minor;
        CUCHECK( cuDeviceGetAttribute(&minor, CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MINOR, peer->gpu_dev) );

        CUfunction func = gds_load_kernel(major, minor, "krn1snd2wait");
        GDS_ASSERT(func);
        peer->kernels.krn1snd2wait = func;

        func = gds_load_kernel(major, minor, "krnsetsndpar");
        GDS_ASSERT(func);
        peer->kernels.krnsetsndpar = func;
        
        gpu_registered[gpu_id] = true;

        gds_dbg("peer_attr: peer_id=%" PRIx64 "\n", peer->attr.peer_id);
}

//-----------------------------------------------------------------------------

static int gds_register_peer(CUdevice dev, CUcontext ctx, unsigned gpu_id, gds_peer **p_peer, gds_peer_attr **p_peer_attr)
{
        int ret = 0;

        gds_dbg("GPU%u: registering peer\n", gpu_id);
        
        if (gpu_id >= max_gpus) {
                gds_err("invalid gpu_id %d\n", gpu_id);
                return EINVAL;
        }

        gds_peer *peer = &gpu_peer[gpu_id];

        if (gpu_registered[gpu_id]) {
                gds_dbg("gds_peer for GPU%u already initialized\n", gpu_id);
                GDS_ASSERT(peer->gpu_ctx == ctx);
        } else {
                gds_init_peer(peer, dev, ctx, gpu_id);
        }

        if (p_peer)
                *p_peer = peer;

        if (p_peer_attr)
                *p_peer_attr = &peer->attr;

        return ret;
}

//-----------------------------------------------------------------------------

static void gds_ordinal_from_device(CUdevice dev, unsigned &gpu_id)
{
        int count;
        CUCHECK(cuDeviceGetCount(&count));
        // FIXME: this is super ugly and may break in the future
        int ordinal = static_cast<int>(dev);
        GDS_ASSERT(ordinal >= 0 && ordinal < count);
        gpu_id = (unsigned)ordinal;
        gds_dbg("gpu_id=%u for dev=%d\n", gpu_id, dev);
}

//-----------------------------------------------------------------------------

static int gds_device_from_current_context(CUdevice &dev)
{
        CUCHECK(cuCtxGetDevice(&dev));
        return 0;
}

//-----------------------------------------------------------------------------

static int gds_current_context(CUcontext &ctx)
{
        CUCHECK(cuCtxGetCurrent(&ctx));
        return 0;
}

//-----------------------------------------------------------------------------

static int gds_device_from_context(CUcontext ctx, CUcontext cur_ctx, CUdevice &dev)
{
        // if cur != ctx then push ctx
        if (ctx != cur_ctx)
                CUCHECK(cuCtxPushCurrent(ctx));
        gds_device_from_current_context(dev);
        // if pushed then pop ctx, to restore original context
        if (ctx != cur_ctx) {
                CUcontext top_ctx;
                CUCHECK(cuCtxPopCurrent(&top_ctx));
                assert(top_ctx == ctx);
        }
        return 0;
}

//-----------------------------------------------------------------------------

static int gds_device_context_from_stream(CUstream stream, CUdevice &dev, CUcontext &ctx)
{
        int ret;
        CUcontext cur_ctx, stream_ctx;
        ret = gds_current_context(cur_ctx);
        if (ret)
                goto out;
#if CUDA_VERSION >= 9020
        CUCHECK(cuStreamGetCtx(stream, &stream_ctx));
#else
        // we assume the stream is associated to the current context
        stream_ctx = cur_ctx;
#endif
        ctx = stream_ctx;
        ret = gds_device_from_context(stream_ctx, cur_ctx, dev);
out:
        return ret;
}

//-----------------------------------------------------------------------------

static int gds_register_peer_by_ordinal(unsigned gpu_id, gds_peer **p_peer, gds_peer_attr **p_peer_attr)
{
        CUdevice dev;
        CUcontext cur_ctx;
        CUdevice cur_dev;
        CUCHECK(cuDeviceGet(&dev, gpu_id));
        CUCHECK(cuCtxGetDevice(&cur_dev));
        if (cur_dev == dev) {
                gds_current_context(cur_ctx);
                return gds_register_peer(cur_dev, cur_ctx, gpu_id, p_peer, p_peer_attr);
        } else {
                gds_err("attempt at registering a GPU other than the one associated to the current context\n");
                return EINVAL;
        }
}

//-----------------------------------------------------------------------------

static int gds_register_peer_by_dev(CUdevice dev, CUcontext ctx, gds_peer **p_peer, gds_peer_attr **p_peer_attr)
{
        unsigned gpu_id;
        gds_ordinal_from_device(dev, gpu_id);
        return gds_register_peer(dev, ctx, gpu_id, p_peer, p_peer_attr);
}

//-----------------------------------------------------------------------------

gds_peer *peer_from_stream(CUstream stream)
{
        CUdevice dev = -1;
        CUcontext ctx;
        gds_peer *peer = NULL;

        if (stream != NULL && stream != CU_STREAM_LEGACY && stream != CU_STREAM_PER_THREAD) {
                // this a user stream
                gds_device_context_from_stream(stream, dev, ctx);
        } else {
                // this is one of the pre-defined streams
                gds_current_context(ctx);
                gds_device_from_current_context(dev);
        }

        // look for pre-registered GPU
        for(unsigned gpu_id=0; gpu_id<max_gpus; ++gpu_id) {
                if (gpu_registered[gpu_id] && (gpu_peer[gpu_id].gpu_dev == dev)) {
                        peer = &gpu_peer[gpu_id];
                        break;
                }
        }
        // otherwise, register this GPU
        if (!peer) {
                gds_peer_attr *peer_attr = NULL;
                int ret = gds_register_peer_by_dev(dev, ctx, &peer, &peer_attr);
                if (ret) {
                        gds_err("error %d while registering GPU dev=%d\n", ret, dev);
                        return NULL;
                }
        }
out:
        return peer;
}

//-----------------------------------------------------------------------------

static ibv_exp_res_domain *gds_create_res_domain(struct ibv_context *context)
{
        if (!context) {
                gds_err("invalid context");
                return NULL;
        }

        ibv_exp_res_domain_init_attr res_domain_attr;
        memset(&res_domain_attr, 0, sizeof(res_domain_attr));

        res_domain_attr.comp_mask |= IBV_EXP_RES_DOMAIN_THREAD_MODEL;
        res_domain_attr.thread_model = IBV_EXP_THREAD_SINGLE;

        ibv_exp_res_domain *res_domain = ibv_exp_create_res_domain(context, &res_domain_attr);
        if (!res_domain) {
                gds_warn("Can't create resource domain\n");
        }

        return res_domain;
}

//-----------------------------------------------------------------------------

static struct gds_cq *
gds_create_cq_internal(struct ibv_context *context, int cqe,
                        void *cq_context, struct ibv_comp_channel *channel,
                        int comp_vector, int gpu_id, gds_alloc_cq_flags_t flags,
                        struct ibv_exp_res_domain * res_domain)
{
        struct gds_cq *gcq = NULL;
        ibv_exp_cq_init_attr attr;
        gds_peer *peer = NULL;
        gds_peer_attr *peer_attr = NULL;
        int ret=0;

        if(!context)
        {
            gds_dbg("Invalid input context\n");
            return NULL;
        }

        gcq = (struct gds_cq*)calloc(1, sizeof(struct gds_cq));
        if (!gcq) {
            gds_err("cannot allocate memory\n");
            return NULL;
        }

        //Here we need to recover peer and peer_attr pointers to set alloc_type and alloc_flags
        //before ibv_exp_create_cq
        ret = gds_register_peer_by_ordinal(gpu_id, &peer, &peer_attr);
        if (ret) {
            gds_err("error %d while registering GPU peer\n", ret);
            return NULL;
        }
        assert(peer);
        assert(peer_attr);

        peer->alloc_type = gds_peer::CQ;
        peer->alloc_flags = flags;

        attr.comp_mask = IBV_EXP_CQ_INIT_ATTR_PEER_DIRECT;
        attr.flags = 0; // see ibv_exp_cq_create_flags
        attr.peer_direct_attrs = peer_attr;
        if (res_domain) {
            gds_dbg("using peer->res_domain %p for CQ\n", res_domain);
            attr.res_domain = res_domain;
            attr.comp_mask |= IBV_EXP_CQ_INIT_ATTR_RES_DOMAIN;
        }
        
        int old_errno = errno;
        gcq->cq = ibv_exp_create_cq(context, cqe, cq_context, channel, comp_vector, &attr);
        if (!gcq->cq) {
            gds_err("error %d in ibv_exp_create_cq, old errno %d\n", errno, old_errno);
            return NULL;
        }

        return gcq;
}

//Note: general create cq function, not really used for now!
struct gds_cq *
gds_create_cq(struct ibv_context *context, int cqe,
              void *cq_context, struct ibv_comp_channel *channel,
              int comp_vector, int gpu_id, gds_alloc_cq_flags_t flags)
{
        int ret = 0;
        struct gds_cq *gcq = NULL;
        //TODO: leak of res_domain
        struct ibv_exp_res_domain * res_domain;
        gds_dbg("cqe=%d gpu_id=%d cq_flags=%08x\n", cqe, gpu_id, flags);

        gds_peer *peer = NULL;
        gds_peer_attr *peer_attr = NULL;
        ret = gds_register_peer_by_ordinal(gpu_id, &peer, &peer_attr);
        if (ret) {
                gds_err("error %d while registering GPU peer\n", ret);
                return NULL;
        }
        assert(peer);
        assert(peer_attr);

        peer->alloc_type = gds_peer::CQ;
        peer->alloc_flags = flags;

        res_domain = gds_create_res_domain(context);
        if (res_domain)
            gds_dbg("using res_domain %p\n", res_domain);
        else
            gds_warn("NOT using res_domain\n");

        
        gcq = gds_create_cq_internal(context, cqe, cq_context, channel, comp_vector, gpu_id, flags, res_domain);

        if (!gcq) {
            gds_err("error in gds_create_cq_internal\n");
            return NULL;
        }

        return gcq;
}

//-----------------------------------------------------------------------------


int gds_add_dv_qp_ctx(struct gds_qp* gqp){
        struct mlx5dv_obj dv_obj = {};
        struct mlx5dv_qp* dv_qp = (struct mlx5dv_qp *)malloc(sizeof(struct mlx5dv_qp));
        memset((void *)&dv_obj, 0, sizeof(struct mlx5dv_obj));        

        dv_obj.qp.in = gqp->qp;
        dv_obj.qp.out = dv_qp;
        int ret = mlx5dv_init_obj(&dv_obj, MLX5DV_OBJ_QP);

        if (ret)
          return ret; 

        gqp->swq_cnt = 0;
        gqp->swq_size = dv_qp->sq.wqe_cnt;
        gqp->swq = dv_qp->sq.buf;
        gqp->swq_stride = dv_qp->sq.stride;
        free(dv_qp);
        gds_dbg("extracted dv_qp context=%p\n", gqp);
        return 0;
}

//-----------------------------------------------------------------------------

struct gds_qp*
gds_create_qp(struct ibv_pd *pd, struct ibv_context *context,
                                gds_qp_init_attr_t *qp_attr, int gpu_id, int flags)
{
        int ret = 0;
        struct gds_qp *gqp = NULL;
        struct ibv_qp *qp = NULL;
        struct gds_cq *rx_gcq = NULL, *tx_gcq = NULL;
        gds_peer *peer = NULL;
        gds_peer_attr *peer_attr = NULL;
        int old_errno = errno;

        gds_dbg("pd=%p context=%p gpu_id=%d flags=%08x current errno=%d\n", pd, context, gpu_id, flags, errno);
        assert(pd);
        assert(context);
        assert(qp_attr);

        if (flags & ~(GDS_CREATE_QP_WQ_ON_GPU|GDS_CREATE_QP_TX_CQ_ON_GPU|GDS_CREATE_QP_RX_CQ_ON_GPU|GDS_CREATE_QP_WQ_DBREC_ON_GPU)) {
                gds_err("invalid flags");
                return NULL;
        }

        gqp = (struct gds_qp*)calloc(1, sizeof(struct gds_qp));
        if (!gqp) {
            gds_err("cannot allocate memory\n");
            return NULL;
        }

        gqp->dev_context=context;

        // peer registration
        gds_dbg("before gds_register_peer_ex\n");
        ret = gds_register_peer_by_ordinal(gpu_id, &peer, &peer_attr);
        if (ret) {
            gds_err("error %d in gds_register_peer_ex\n", ret);
            goto err;
        }

        gqp->res_domain = gds_create_res_domain(context);
        if (gqp->res_domain)
            gds_dbg("using gqp->res_domain %p\n", gqp->res_domain);
        else
            gds_warn("NOT using gqp->res_domain\n");

        tx_gcq = gds_create_cq_internal(context, qp_attr->cap.max_send_wr, NULL, NULL, 0, gpu_id, 
                              (flags & GDS_CREATE_QP_TX_CQ_ON_GPU) ? GDS_ALLOC_CQ_ON_GPU : GDS_ALLOC_CQ_DEFAULT, 
                              gqp->res_domain);
        if (!tx_gcq) {
                ret = errno;
                gds_err("error %d while creating TX CQ, old_errno=%d\n", ret, old_errno);
                goto err;
        }

        rx_gcq = gds_create_cq_internal(context, qp_attr->cap.max_recv_wr, NULL, NULL, 0, gpu_id, 
                              (flags & GDS_CREATE_QP_RX_CQ_ON_GPU) ? GDS_ALLOC_CQ_ON_GPU : GDS_ALLOC_CQ_DEFAULT, 
                              gqp->res_domain);
        if (!rx_gcq) {
                ret = errno;
                gds_err("error %d while creating RX CQ\n", ret);
                goto err;
        }

        // peer registration
        qp_attr->send_cq = tx_gcq->cq;
        qp_attr->recv_cq = rx_gcq->cq;
        qp_attr->pd = pd;
        qp_attr->comp_mask |= IBV_EXP_QP_INIT_ATTR_PD;

        peer->alloc_type = gds_peer::WQ;
        peer->alloc_flags = GDS_ALLOC_WQ_DEFAULT | GDS_ALLOC_DBREC_DEFAULT;
        if (flags & GDS_CREATE_QP_WQ_ON_GPU) {
                gds_err("error, QP WQ on GPU is not supported yet\n");
                goto err;
        }
        if (flags & GDS_CREATE_QP_WQ_DBREC_ON_GPU) {
                gds_warn("QP WQ DBREC on GPU\n");
                peer->alloc_flags |= GDS_ALLOC_DBREC_ON_GPU;
        }        
        qp_attr->comp_mask |= IBV_EXP_QP_INIT_ATTR_PEER_DIRECT;
        qp_attr->peer_direct_attrs = peer_attr;

        qp = ibv_exp_create_qp(context, qp_attr);
        if (!qp)  {
                ret = EINVAL;
                gds_err("error in ibv_exp_create_qp\n");
                goto err;
        }

        gqp->qp = qp;
        gqp->send_cq.cq = qp->send_cq;
        gqp->send_cq.curr_offset = 0;
        gqp->recv_cq.cq = qp->recv_cq;
        gqp->recv_cq.curr_offset = 0;


        ret = gds_add_dv_qp_ctx(gqp);
        if (!ret){
          ret = EINVAL;
          gds_err("error in gds_add_dv_qp_ctx\n");
          goto err;
        }

        gds_dbg("created gds_qp=%p\n", gqp);
        return gqp;

err:
        gds_dbg("destroying QP\n");
        gds_destroy_qp(gqp);

        return NULL;
}

//-----------------------------------------------------------------------------

int gds_destroy_qp(struct gds_qp *gqp)
{
        int retcode = 0;
        int ret;
        
        if(!gqp) return retcode;

        if(gqp->qp)
        {
            ret = ibv_destroy_qp(gqp->qp);
            if (ret) {
                    gds_err("error %d in destroy_qp\n", ret);
                    retcode = ret;
            }            
        }

        if(gqp->send_cq.cq)
        {
            ret = ibv_destroy_cq(gqp->send_cq.cq);
            if (ret) {
                    gds_err("error %d in destroy_cq send_cq\n", ret);
                    retcode = ret;
            }
        }

        if(gqp->recv_cq.cq)
        {
            ret = ibv_destroy_cq(gqp->recv_cq.cq);
            if (ret) {
                    gds_err("error %d in destroy_cq recv_cq\n", ret);
                    retcode = ret;
            }
        }

        if(gqp->res_domain) {
            struct ibv_exp_destroy_res_domain_attr attr; //IBV_EXP_DESTROY_RES_DOMAIN_RESERVED
            attr.comp_mask=0;
            ret = ibv_exp_destroy_res_domain(gqp->dev_context, gqp->res_domain, &attr);
            if (ret) {
                    gds_err("ibv_exp_destroy_res_domain error %d: %s\n", ret, strerror(ret));
                    retcode = ret;
            }            
        }

        free(gqp);

        return retcode;
}

//-----------------------------------------------------------------------------

int gds_query_param(gds_param_t param, int *value)
{
        int ret = 0;
        if (!value)
                return EINVAL;

        switch (param) {
        case GDS_PARAM_VERSION:
                *value = (GDS_API_MAJOR_VERSION << 16)|GDS_API_MINOR_VERSION;
                break;
        default:
                ret = EINVAL;
                break;
        };
        return ret;
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
