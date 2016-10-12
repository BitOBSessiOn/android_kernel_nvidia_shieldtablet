/*
 * Copyright (c) 2012-2016 NVIDIA Corporation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 */

#include <linux/atomic.h>
#include <linux/uaccess.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/fs.h>
#include <linux/printk.h>
#include <linux/ioctl.h>
#include <linux/sched.h>
#include <linux/mm.h>
#include <linux/pagemap.h>
#include <asm/smp_plat.h>

#include "ote_protocol.h"

bool verbose_smc;
core_param(verbose_smc, verbose_smc, bool, 0644);

#define VR_AUTH_UUID	{0x0179ED96, 0x45A81ADB, 0x089DC68D, 0xBB520279}
#define SET_RESULT(req, r, ro)	{ req->result = r; req->result_origin = ro; }

static int te_pin_user_pages(void *buffer, size_t size,
		struct page ***pages_ptr, uint32_t buf_type, uint32_t nr_pages)
{
	int ret = 0;
	struct page **pages = NULL;
	bool writable;
	int i;

	pages = kzalloc(nr_pages * sizeof(struct page *), GFP_KERNEL);
	if (!pages)
		return -ENOMEM;

	writable = (buf_type == TE_PARAM_TYPE_MEM_RW ||
		buf_type == TE_PARAM_TYPE_PERSIST_MEM_RW);

	down_read(&current->mm->mmap_sem);
	ret = get_user_pages(current, current->mm, (unsigned long)buffer,
			nr_pages, writable,
			0, pages, NULL);

	up_read(&current->mm->mmap_sem);

	if (ret < nr_pages) {
		nr_pages = ret;
		pr_err("%s: Error %d in get_user_pages\n", __func__, ret);
		goto error;
	}

	*pages_ptr = pages;

	return OTE_SUCCESS;
error:
	for (i = 0; i < nr_pages; i++)
		put_page(pages[i]);
	kfree(pages);
	return OTE_ERROR_OUT_OF_MEMORY;
}

static void te_release_mem_buffer(struct te_shmem_desc *shmem_desc)
{
	uint32_t i;

	list_del(&shmem_desc->list);
	for (i = 0; i < shmem_desc->nr_pages; i++) {
		if ((shmem_desc->type == TE_PARAM_TYPE_MEM_RW) ||
			(shmem_desc->type == TE_PARAM_TYPE_PERSIST_MEM_RW))
			set_page_dirty_lock(shmem_desc->pages[i]);
		put_page(shmem_desc->pages[i]);
	}
	kfree(shmem_desc->pages);
	kfree(shmem_desc);
}

static void te_release_mem_buffers(struct list_head *buflist)
{
	struct te_shmem_desc *shmem_desc, *tmp_shmem_desc;

	list_for_each_entry_safe(shmem_desc, tmp_shmem_desc, buflist, list) {
		te_release_mem_buffer(shmem_desc);
	}
}

static void te_activate_persist_mem_buffers(struct te_session *session)
{
	struct te_shmem_desc *shmem_desc, *tmp_shmem_desc;

	/* move persist mem buffers from inactive list to active list */
	list_for_each_entry_safe(shmem_desc, tmp_shmem_desc,
		&session->inactive_persist_shmem_list, list) {

		list_move_tail(&shmem_desc->list, &session->persist_shmem_list);
	}
}

static int te_prep_mem_buffer(uint32_t session_id,
		void *buffer, size_t size, uint32_t buf_type,
		struct te_session *session)
{
	struct page **pages = NULL;
	struct te_shmem_desc *shmem_desc = NULL;
	int ret = 0;
	uint32_t nr_pages;

	/* allocate new shmem descriptor */
	shmem_desc = kzalloc(sizeof(struct te_shmem_desc), GFP_KERNEL);
	if (!shmem_desc) {
		pr_err("%s: memory allocation failed\n", __func__);
		ret = OTE_ERROR_OUT_OF_MEMORY;
		goto error;
	}

	nr_pages = (((uintptr_t)buffer & (PAGE_SIZE - 1)) +
				(size + PAGE_SIZE - 1)) >> PAGE_SHIFT;
	/* pin pages */
	ret = te_pin_user_pages(buffer, size, &pages, buf_type, nr_pages);
	if (ret != OTE_SUCCESS) {
		pr_err("%s: te_pin_user_pages failed (%d)\n", __func__,
			nr_pages);
		kfree(shmem_desc);
		goto error;
	}

	/* initialize shmem descriptor */
	INIT_LIST_HEAD(&(shmem_desc->list));
	shmem_desc->buffer = buffer;
	shmem_desc->size = size;
	shmem_desc->nr_pages = nr_pages;
	shmem_desc->pages = pages;

	/* add shmem descriptor to proper list */
	if ((buf_type == TE_PARAM_TYPE_MEM_RO) ||
		(buf_type == TE_PARAM_TYPE_MEM_RW))
		list_add_tail(&shmem_desc->list, &session->temp_shmem_list);
	else {
		list_add_tail(&shmem_desc->list,
			&session->inactive_persist_shmem_list);
	}

	return OTE_SUCCESS;
error:
	return ret;
}

static int te_prep_mem_buffers(struct te_request *request,
			struct te_session *session)
{
	uint32_t i;
	int ret = OTE_SUCCESS;
	struct te_oper_param *params;

	params = (struct te_oper_param *)(uintptr_t)request->params;
	for (i = 0; i < request->params_size; i++) {
		switch (params[i].type) {
		case TE_PARAM_TYPE_NONE:
		case TE_PARAM_TYPE_INT_RO:
		case TE_PARAM_TYPE_INT_RW:
			break;
		case TE_PARAM_TYPE_MEM_RO:
		case TE_PARAM_TYPE_MEM_RW:
		case TE_PARAM_TYPE_PERSIST_MEM_RO:
		case TE_PARAM_TYPE_PERSIST_MEM_RW:
			ret = te_prep_mem_buffer(request->session_id,
				(void *)(uintptr_t)params[i].u.Mem.base,
				params[i].u.Mem.len,
				params[i].type,
				session);
			if (ret != OTE_SUCCESS) {
				pr_err("%s failed with err (%d)\n",
					__func__, ret);
				ret = OTE_ERROR_BAD_PARAMETERS;
				goto error;
			}
			break;
		default:
			pr_err("%s: OTE_ERROR_BAD_PARAMETERS\n", __func__);
			ret = OTE_ERROR_BAD_PARAMETERS;
			break;
		}
	}
	return OTE_SUCCESS;
error:
	/* release mem buffers */
	te_release_mem_buffers(&session->temp_shmem_list);
	te_release_mem_buffers(&session->inactive_persist_shmem_list);
	return ret;
}

static struct te_session *te_get_session(struct tlk_context *context,
					uint32_t session_id)
{
	struct te_session *session, *tmp_session;

	list_for_each_entry_safe(session, tmp_session,
		&context->session_list, list) {
		if (session->session_id == session_id)
			return session;
	}

	return NULL;
}

#ifdef CONFIG_SMP
cpumask_t saved_cpu_mask;
static long switch_cpumask_to_cpu0(void)
{
	long ret;
	cpumask_t local_cpu_mask = CPU_MASK_NONE;

	cpu_set(0, local_cpu_mask);
	cpumask_copy(&saved_cpu_mask, tsk_cpus_allowed(current));
	ret = sched_setaffinity(0, &local_cpu_mask);
	if (ret)
		pr_err("%s: sched_setaffinity #1 -> 0x%lX", __func__, ret);

	return ret;
}

static void restore_cpumask(void)
{
	long ret = sched_setaffinity(0, &saved_cpu_mask);
	if (ret)
		pr_err("%s: sched_setaffinity #2 -> 0x%lX", __func__, ret);
}
#else
static inline long switch_cpumask_to_cpu0(void) { return 0; };
static inline void restore_cpumask(void) {};
#endif

struct tlk_smc_work_args {
	uint32_t arg0;
	uintptr_t arg1;
	uint32_t arg2;
};

static long tlk_generic_smc_on_cpu0(void *args)
{
	struct tlk_smc_work_args *work;
	int callback_status = 0;
	uint32_t retval;

	work = (struct tlk_smc_work_args *)args;
	retval = _tlk_generic_smc(work->arg0, work->arg1, work->arg2);

	while (retval == TE_ERROR_PREEMPT_BY_IRQ ||
	       retval == TE_ERROR_PREEMPT_BY_FS) {
		if (retval == TE_ERROR_PREEMPT_BY_FS)
			callback_status = tlk_ss_op();
		retval = _tlk_generic_smc(TE_SMC_RESTART, callback_status, 0);
	}

	/* Print TLK logs if any */
	ote_print_logs();

	return retval;
}

/*
 * This routine is called both from normal threads and worker threads.
 * The worker threads are per-cpu and have PF_NO_SETAFFINITY set, so
 * any calls to sched_setaffinity will fail.
 *
 * If it's a worker thread on CPU0, just invoke the SMC directly. If
 * it's running on a non-CPU0, use work_on_cpu() to schedule the SMC
 * on CPU0.
 */
uint32_t send_smc(uint32_t arg0, uintptr_t arg1, uintptr_t arg2)
{
	long ret;
	struct tlk_smc_work_args work_args;

	work_args.arg0 = arg0;
	work_args.arg1 = arg1;
	work_args.arg2 = arg2;

	if (current->flags &
	    (PF_WQ_WORKER | PF_NO_SETAFFINITY | PF_KTHREAD)) {
		int cpu = cpu_logical_map(get_cpu());
		put_cpu();

		/* workers don't change CPU. depending on the CPU, execute
		 * directly or sched work */
		if (cpu == 0 && (current->flags & PF_WQ_WORKER))
			return tlk_generic_smc_on_cpu0(&work_args);
		else
			return work_on_cpu(0,
					tlk_generic_smc_on_cpu0, &work_args);
	}

	/* switch to CPU0 */
	ret = switch_cpumask_to_cpu0();
	if (ret) {
		/* not able to switch, schedule work on CPU0 */
		ret = work_on_cpu(0, tlk_generic_smc_on_cpu0, &work_args);
	} else {
		/* switched to CPU0 */
		ret = tlk_generic_smc_on_cpu0(&work_args);
		restore_cpumask();
	}

	return ret;
}

/*
 * Do an SMC call
 */
static void do_smc(struct te_request *request, struct tlk_device *dev)
{
	uint32_t smc_args;
	uint32_t smc_params = 0;

	smc_args = (char *)request - dev->req_param_buf;
	if (request->params) {
		smc_params =
			(char *)(uintptr_t)request->params - dev->req_param_buf;
	}

	(void)send_smc(request->type, smc_args, smc_params);
}

/*
 * VPR programming SMC
 */
int te_set_vpr_params(void *vpr_base, size_t vpr_size)
{
	uint32_t retval;

	/* Share the same lock used when request is send from user side */
	mutex_lock(&smc_lock);

	retval = send_smc(TE_SMC_PROGRAM_VPR, (uintptr_t)vpr_base, vpr_size);

	mutex_unlock(&smc_lock);

	if (retval != OTE_SUCCESS) {
		pr_err("%s: smc failed err (0x%x)\n", __func__, retval);
		return -EINVAL;
	}
	return 0;
}
EXPORT_SYMBOL(te_set_vpr_params);

void te_restore_keyslots(void)
{
	uint32_t retval;

	/* Share the same lock used when request is send from user side */
	mutex_lock(&smc_lock);

	retval = send_smc(TE_SMC_TA_EVENT, TA_EVENT_RESTORE_KEYS, 0);

	mutex_unlock(&smc_lock);

	if (retval != OTE_SUCCESS) {
		pr_err("%s: smc failed err (0x%x)\n", __func__, retval);
	}
}
EXPORT_SYMBOL(te_restore_keyslots);

/*
 * VRR Set Buffer
 *
 * Called from the DC driver and implemented as a monitor fastcall
 * to avoid taking the smc_lock. This call passes in the physical
 * address for the shared memory buffer.
 */
int te_vrr_set_buf(phys_addr_t addr)
{
	return _tlk_generic_smc(TE_SMC_VRR_SET_BUF, addr, 0);
}
EXPORT_SYMBOL(te_vrr_set_buf);

/*
 * VRR Sec
 *
 * Called from the DC driver and implemented as a monitor fastcall
 * to avoid taking the smc_lock.
 */
void te_vrr_sec(void)
{
	_tlk_generic_smc(TE_SMC_VRR_SEC, 0, 0);
}
EXPORT_SYMBOL(te_vrr_sec);

/*
 * Open session SMC (supporting client-based te_open_session() calls)
 */
void te_open_session(struct te_opensession *cmd,
		    struct te_request *request,
		    struct tlk_context *context)
{
	struct te_session *session;
	int ret;

	session = kzalloc(sizeof(struct te_session), GFP_KERNEL);
	if (!session) {
		SET_RESULT(request, OTE_ERROR_OUT_OF_MEMORY,
			OTE_RESULT_ORIGIN_API);
		return;
	}

	INIT_LIST_HEAD(&session->list);
	INIT_LIST_HEAD(&session->temp_shmem_list);
	INIT_LIST_HEAD(&session->inactive_persist_shmem_list);
	INIT_LIST_HEAD(&session->persist_shmem_list);

	request->type = TE_SMC_OPEN_SESSION;

	ret = te_prep_mem_buffers(request, session);
	if (ret != OTE_SUCCESS) {
		pr_err("%s: te_prep_mem_buffers failed err (0x%x)\n",
			__func__, ret);
		SET_RESULT(request, ret, OTE_RESULT_ORIGIN_API);
		kfree(session);
		return;
	}

	memcpy(&request->dest_uuid,
	       &cmd->dest_uuid,
	       sizeof(struct te_service_id));

	pr_info("OPEN_CLIENT_SESSION: 0x%x 0x%x 0x%x 0x%x\n",
		request->dest_uuid[0],
		request->dest_uuid[1],
		request->dest_uuid[2],
		request->dest_uuid[3]);

	do_smc(request, context->dev);

	if (request->result)
		goto error;

	/* release temporary mem buffers */
	te_release_mem_buffers(&session->temp_shmem_list);

	/* move persistent mem buffers to active list */
	te_activate_persist_mem_buffers(session);

	/* save off session_id and add to list */
	session->session_id = request->session_id;
	list_add_tail(&session->list, &context->session_list);

	return;
error:
	/* release buffers allocated in te_prep_mem_buffers */
	te_release_mem_buffers(&session->temp_shmem_list);
	te_release_mem_buffers(&session->inactive_persist_shmem_list);

	kfree(session);
}

/*
 * Close session SMC (supporting client-based te_close_session() calls)
 */
void te_close_session(struct te_closesession *cmd,
		     struct te_request *request,
		     struct tlk_context *context)
{
	struct te_session *session;

	request->session_id = cmd->session_id;
	request->type = TE_SMC_CLOSE_SESSION;

	do_smc(request, context->dev);
	if (request->result)
		pr_info("%s: error closing session: 0x%08x\n",
			__func__, request->result);

	session = te_get_session(context, cmd->session_id);
	if (!session) {
		pr_info("%s: session_id not found: 0x%x\n",
			__func__, cmd->session_id);
		return;
	}

	/* free session state */
	te_release_mem_buffers(&session->persist_shmem_list);
	list_del(&session->list);
	kfree(session);
}

/*
 * Launch operation SMC (supporting client-based te_launch_operation() calls)
 */
void te_launch_operation(struct te_launchop *cmd,
			struct te_request *request,
			struct tlk_context *context)
{
	struct te_session *session;
	int ret;

	session = te_get_session(context, cmd->session_id);
	if (!session) {
		pr_info("%s: session_id not found: 0x%x\n",
			__func__, cmd->session_id);
		SET_RESULT(request, OTE_ERROR_BAD_PARAMETERS,
				OTE_RESULT_ORIGIN_API);
		return;
	}

	request->session_id = cmd->session_id;
	request->command_id = cmd->operation.command;
	request->type = TE_SMC_LAUNCH_OPERATION;

	ret = te_prep_mem_buffers(request, session);
	if (ret != OTE_SUCCESS) {
		pr_err("%s: te_prep_mem_buffers failed err (0x%x)\n",
			__func__, ret);
		SET_RESULT(request, ret, OTE_RESULT_ORIGIN_API);
		return;
	}

	do_smc(request, context->dev);

	if (request->result)
		goto error;

	/* move persistent mem buffers to active list */
	te_activate_persist_mem_buffers(session);

	/* release temporary mem buffers */
	te_release_mem_buffers(&session->temp_shmem_list);

	return;

error:
	/* release buffers allocated in te_prep_mem_buffers */
	te_release_mem_buffers(&session->temp_shmem_list);
	te_release_mem_buffers(&session->inactive_persist_shmem_list);
}

void te_authenticate_vrr(u8 *buf_ptr, u32 buflen)
{
	u32 i, no_of_params = 1;
	struct te_request *request;
	struct te_oper_param user_param;
	struct te_oper_param *param_array;
	struct te_oper_param *params = NULL;
	struct te_cmd_req_desc *cmd_desc = NULL;
	u32 session_id, vrr_auth_uuid[4] = VR_AUTH_UUID;

	mutex_lock(&smc_lock);

	/* Open & submit the work to SMC */
	cmd_desc = NULL;
	params = NULL;
	no_of_params =  1;

	cmd_desc = te_get_free_cmd_desc(&tlk_dev);
	params = te_get_free_params(&tlk_dev, no_of_params);

	if (!cmd_desc || !params) {
		pr_err("failed to get cmd_desc/params\n");
		goto error;
	}

	/* Request and parameter are prepared for VRR authenticaiton */
	request = cmd_desc->req_addr;
	memset(request, 0, sizeof(struct te_request));
	request->params = (uintptr_t)params;
	request->params_size = no_of_params;
	request->type = TE_SMC_OPEN_SESSION;

	user_param.index = 0;
	user_param.u.Mem.len = buflen;
	user_param.type = TE_PARAM_TYPE_MEM_RW;
	user_param.u.Mem.type = TE_MEM_TYPE_NS_KERNEL;
	user_param.u.Mem.base = (uint64_t)(uintptr_t)buf_ptr;
	memcpy(request->dest_uuid, vrr_auth_uuid, sizeof(vrr_auth_uuid));

	param_array = (struct te_oper_param *)(uintptr_t)request->params;

	for (i = 0; i < no_of_params; i++)
		memcpy(param_array + i, &user_param, sizeof(struct te_oper_param));

	do_smc(request, &tlk_dev);
	session_id = request->session_id;

	if (request->result) {
		pr_err("%s: error opening session: 0x%08x\n",
			__func__, request->result);
		goto error;
	}

	/* Close the session */
	request = cmd_desc->req_addr;
	memset(request, 0, sizeof(struct te_request));

	request->type = TE_SMC_CLOSE_SESSION;
	request->session_id = session_id;
	memcpy(request->dest_uuid, vrr_auth_uuid, sizeof(vrr_auth_uuid));

	do_smc(request, &tlk_dev);

	if (request->result) {
		pr_err("%s: error closing session: 0x%08x\n",
			__func__, request->result);
	}

error:
	if (cmd_desc)
		te_put_used_cmd_desc(&tlk_dev, cmd_desc);

	if (params)
		te_put_free_params(&tlk_dev, params, no_of_params);

	mutex_unlock(&smc_lock);
}
EXPORT_SYMBOL(te_authenticate_vrr);
