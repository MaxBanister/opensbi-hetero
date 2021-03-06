/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2019 Western Digital Corporation or its affiliates.
 *
 * Authors:
 *   Anup Patel <anup.patel@wdc.com>
 *   Nick Kossifidis <mick@ics.forth.gr>
 */

#include <sbi/riscv_asm.h>
#include <sbi/riscv_atomic.h>
#include <sbi/riscv_barrier.h>
#include <sbi/sbi_bitops.h>
#include <sbi/sbi_domain.h>
#include <sbi/sbi_error.h>
#include <sbi/sbi_hart.h>
#include <sbi/sbi_hsm.h>
#include <sbi/sbi_init.h>
#include <sbi/sbi_ipi.h>
#include <sbi/sbi_platform.h>
#include <sbi/sbi_pmu.h>
#include <sbi/sbi_string.h>
#include <sbi/sbi_tlb.h>
#include <sbi/sbi_fifo.h>
#include <sbi/sbi_trap.h>
#include <sbi/sbi_console.h>
#include <sbi/sbi_unpriv.h>

struct sbi_ipi_data {
	unsigned long ipi_type;
};

static unsigned long ipi_data_off;
static unsigned long task_fifo_off;
static unsigned long task_fifo_mem_off;
unsigned long task_data_off;
static const struct sbi_ipi_device *ipi_dev = NULL;
static const struct sbi_ipi_event_ops *ipi_ops_array[SBI_IPI_EVENT_MAX];
/* In the future, extend this to support multi-hart */
unsigned long accelerator_hart = 0;

static int sbi_ipi_send(struct sbi_scratch *scratch, u32 remote_hartid,
			u32 event, void *data)
{
	int ret;
	struct sbi_scratch *remote_scratch = NULL;
	struct sbi_ipi_data *ipi_data;
	const struct sbi_ipi_event_ops *ipi_ops;

	if ((SBI_IPI_EVENT_MAX <= event) ||
	    !ipi_ops_array[event])
		return SBI_EINVAL;
	ipi_ops = ipi_ops_array[event];

	remote_scratch = sbi_hartid_to_scratch(remote_hartid);
	if (!remote_scratch)
		return SBI_EINVAL;

	ipi_data = sbi_scratch_offset_ptr(remote_scratch, ipi_data_off);

	if (ipi_ops->update) {
		ret = ipi_ops->update(scratch, remote_scratch,
				      remote_hartid, data);
		if (ret < 0)
			return ret;
	}

	/*
	 * Set IPI type on remote hart's scratch area and
	 * trigger the interrupt
	 */
	atomic_raw_set_bit(event, &ipi_data->ipi_type);
	smp_wmb();

	if (ipi_dev && ipi_dev->ipi_send)
		ipi_dev->ipi_send(remote_hartid);

	sbi_pmu_ctr_incr_fw(SBI_PMU_FW_IPI_SENT);

	if (ipi_ops->sync)
		ipi_ops->sync(scratch);

	return 0;
}

/**
 * As this this function only handlers scalar values of hart mask, it must be
 * set to all online harts if the intention is to send IPIs to all the harts.
 * If hmask is zero, no IPIs will be sent.
 */
int sbi_ipi_send_many(ulong hmask, ulong hbase, u32 event, void *data)
{
	int rc;
	ulong i, m;
	struct sbi_domain *dom = sbi_domain_thishart_ptr();
	struct sbi_scratch *scratch = sbi_scratch_thishart_ptr();

	if (hbase != -1UL) {
		rc = sbi_hsm_hart_interruptible_mask(dom, hbase, &m);
		if (rc)
			return rc;
		m &= hmask;

		/* Send IPIs */
		for (i = hbase; m; i++, m >>= 1) {
			if (m & 1UL)
				sbi_ipi_send(scratch, i, event, data);
		}
	} else {
		hbase = 0;
		while (!sbi_hsm_hart_interruptible_mask(dom, hbase, &m)) {
			/* Send IPIs */
			for (i = hbase; m; i++, m >>= 1) {
				if (m & 1UL)
					sbi_ipi_send(scratch, i, event, data);
			}
			hbase += BITS_PER_LONG;
		}
	}

	return 0;
}

int sbi_ipi_event_create(const struct sbi_ipi_event_ops *ops)
{
	int i, ret = SBI_ENOSPC;

	if (!ops || !ops->process)
		return SBI_EINVAL;

	for (i = 0; i < SBI_IPI_EVENT_MAX; i++) {
		if (!ipi_ops_array[i]) {
			ret = i;
			ipi_ops_array[i] = ops;
			break;
		}
	}

	return ret;
}

void sbi_ipi_event_destroy(u32 event)
{
	if (SBI_IPI_EVENT_MAX <= event)
		return;

	ipi_ops_array[event] = NULL;
}

static void sbi_ipi_process_smode(struct sbi_scratch *scratch)
{
	csr_set(CSR_MIP, MIP_SSIP);
}

static struct sbi_ipi_event_ops ipi_smode_ops = {
	.name = "IPI_SMODE",
	.process = sbi_ipi_process_smode,
};

static u32 ipi_smode_event = SBI_IPI_EVENT_MAX;

int sbi_ipi_send_smode(ulong hmask, ulong hbase)
{
	return sbi_ipi_send_many(hmask, hbase, ipi_smode_event, NULL);
}

void sbi_ipi_clear_smode(void)
{
	csr_clear(CSR_MIP, MIP_SSIP);
}

static void sbi_ipi_process_halt(struct sbi_scratch *scratch)
{
	sbi_hsm_hart_stop(scratch, TRUE);
}

static struct sbi_ipi_event_ops ipi_halt_ops = {
	.name = "IPI_HALT",
	.process = sbi_ipi_process_halt,
};

static u32 ipi_halt_event = SBI_IPI_EVENT_MAX;

int sbi_ipi_send_halt(ulong hmask, ulong hbase)
{
	return sbi_ipi_send_many(hmask, hbase, ipi_halt_event, NULL);
}

/*
 * Define ipi ops for linux -> accelerator communication:
 * update: put satp, pid, and regs in fifo queue
 */
static int task_run_update(struct sbi_scratch *scratch,
			  struct sbi_scratch *remote_scratch,
			  u32 remote_hartid, void *data) {

	struct sbi_fifo *rfifo = sbi_scratch_offset_ptr(remote_scratch, task_fifo_off);
	return sbi_fifo_enqueue(rfifo, data);
}

static void task_run_process(struct sbi_scratch *scratch) {
	struct sbi_fifo *rfifo = sbi_scratch_offset_ptr(scratch, task_fifo_off);
	struct task_data *tdata = sbi_scratch_offset_ptr(scratch, task_data_off);
	struct sbi_trap_regs *regs = (struct sbi_trap_regs *)scratch->tmp0;
	struct task_context ctxt;
	int ret = 0;
	unsigned long next_mstatus;

	ret = sbi_fifo_dequeue(rfifo, &ctxt);
	if (ret < 0) {
		sbi_printf("error: could not dequeue from task run\n");
		return;
	}
	/* Override the trap registers, which we don't care about. */
	regs->zero = 0;
	sbi_memcpy(&regs->ra, &ctxt.regs, 31 * 8);
	regs->mepc = ctxt.epc;

	next_mstatus = regs->mstatus;
	next_mstatus = INSERT_FIELD(next_mstatus, MSTATUS_MPP, PRV_U);
	next_mstatus = INSERT_FIELD(next_mstatus, MSTATUS_MPIE, 0);
	regs->mstatus = next_mstatus;

	csr_write(CSR_SATP, ctxt.satp);
	__asm__ __volatile__("sfence.vma" : : : "memory");

	tdata->pid = ctxt.pid;
	tdata->kernel_regs = ctxt.kernel_regs;
	tdata->origin_hart = ctxt.origin_hart;
}

static struct sbi_ipi_event_ops ipi_run_task_ops = {
	.name = "IPI_RUN_TASK",
	.update = task_run_update,
	.process = task_run_process
};

static u32 ipi_run_task_event = SBI_IPI_EVENT_MAX;

int sbi_ipi_send_run_task(struct sbi_scratch *scratch, void *data)
{
	if (accelerator_hart < 0) {
		sbi_printf("error: no accelerator hart found\n");
		return SBI_ENOTSUPP;
	}
	return sbi_ipi_send(scratch, accelerator_hart, ipi_run_task_event, data);
}

/*
 * Define ipi ops for accelerator -> linux communication:
 * update: put task_context struct in linux hart's message queue
 * process: delegate trap down to linux, put 16 in mcause
 */
static int task_restart_update(struct sbi_scratch *scratch,
			  struct sbi_scratch *remote_scratch,
			  u32 remote_hartid, void *data) {

	struct sbi_fifo *rfifo = sbi_scratch_offset_ptr(remote_scratch, task_fifo_off);
	return sbi_fifo_enqueue(rfifo, data);
}

static void task_restart_process(struct sbi_scratch *scratch) {
	struct sbi_trap_info store_trap, ret_trap;
	struct sbi_fifo *fifo = sbi_scratch_offset_ptr(scratch, task_fifo_off);
	struct task_context ctxt;
	unsigned long old_satp;
	struct sbi_trap_regs *regs = (struct sbi_trap_regs *)scratch->tmp0;

	int ret = sbi_fifo_dequeue(fifo, &ctxt);
	if (ret < 0) {
		sbi_printf("error: could not dequeue in restart");
		return;
	}

	/*
	 * We must access the process's address space to store our updated regs,
	 * so we must swap out the satp temporarily, before putting it back.
	 */
	old_satp = csr_read(CSR_SATP);
	csr_write(CSR_SATP, ctxt.satp);

	/* May not be necessary */
	__asm__ __volatile__("sfence.vma" : : : "memory");

	/* Copy updated regs to kernel stack */
	for (int i = 1; i < 32; i++) {
		sbi_store_u64((u64 *)ctxt.kernel_regs + i, ctxt.regs[i-1], &store_trap);
		if (store_trap.cause)
			goto trap_exit;
	}
	sbi_store_u64((u64 *)ctxt.kernel_regs, ctxt.epc, &store_trap);
	if (store_trap.cause)
		goto trap_exit;

	csr_write(CSR_SATP, old_satp);
	__asm__ __volatile__("sfence.vma" : : : "memory");

	ret_trap.epc = regs->mepc;
	ret_trap.cause = (1ULL << (__riscv_xlen - 1)) | 16;
	ret_trap.tval = ctxt.pid;
	/* We can implement H extension support later */
	ret_trap.tval2 = 0;
	ret_trap.tinst = 0;
	ret = sbi_trap_redirect(regs, &ret_trap);
	if (ret)
		sbi_printf("error: trap redirect failed\n");
	return;

trap_exit:
	sbi_printf("error: writing to kernel's address space failed, cause=%lu\n", store_trap.cause);
	csr_write(CSR_SATP, old_satp);
	return;
}

static struct sbi_ipi_event_ops ipi_restart_ops = {
	.name = "IPI_RESTART",
	.update = task_restart_update,
	.process = task_restart_process
};

static u32 ipi_restart_event = SBI_IPI_EVENT_MAX;

int sbi_ipi_send_restart(struct sbi_scratch *scratch,
								 struct task_context *ctxt)
{
	return sbi_ipi_send(scratch, ctxt->origin_hart, ipi_restart_event, ctxt);
}

void sbi_ipi_process(void)
{
	unsigned long ipi_type;
	unsigned int ipi_event;
	const struct sbi_ipi_event_ops *ipi_ops;
	struct sbi_scratch *scratch = sbi_scratch_thishart_ptr();
	struct sbi_ipi_data *ipi_data =
			sbi_scratch_offset_ptr(scratch, ipi_data_off);
	u32 hartid = current_hartid();

	sbi_pmu_ctr_incr_fw(SBI_PMU_FW_IPI_RECVD);
	if (ipi_dev && ipi_dev->ipi_clear)
		ipi_dev->ipi_clear(hartid);

	ipi_type = atomic_raw_xchg_ulong(&ipi_data->ipi_type, 0);
	ipi_event = 0;
	while (ipi_type) {
		if (!(ipi_type & 1UL))
			goto skip;

		ipi_ops = ipi_ops_array[ipi_event];
		if (ipi_ops && ipi_ops->process)
			ipi_ops->process(scratch);

skip:
		ipi_type = ipi_type >> 1;
		ipi_event++;
	};
}

void sbi_ipi_raw_send(u32 target_hart)
{
	if (ipi_dev && ipi_dev->ipi_send)
		ipi_dev->ipi_send(target_hart);
}

const struct sbi_ipi_device *sbi_ipi_get_device(void)
{
	return ipi_dev;
}

void sbi_ipi_set_device(const struct sbi_ipi_device *dev)
{
	if (!dev || ipi_dev)
		return;

	ipi_dev = dev;
}

int sbi_ipi_init(struct sbi_scratch *scratch, bool cold_boot)
{
	int ret;
	struct sbi_ipi_data *ipi_data;
	struct sbi_fifo *task_fifo;
	struct task_context *task_fifo_mem;

	if (cold_boot) {
		ipi_data_off = sbi_scratch_alloc_offset(sizeof(*ipi_data));
		if (!ipi_data_off)
			return SBI_ENOMEM;
		ret = sbi_ipi_event_create(&ipi_smode_ops);
		if (ret < 0)
			return ret;
		ipi_smode_event = ret;
		ret = sbi_ipi_event_create(&ipi_halt_ops);
		if (ret < 0)
			return ret;
		ipi_halt_event = ret;
		/* Accelerator ipi events */
		ret = sbi_ipi_event_create(&ipi_run_task_ops);
		if (ret < 0)
			return ret;
		ipi_run_task_event = ret;
		ret = sbi_ipi_event_create(&ipi_restart_ops);
		if (ret < 0)
			return ret;
		ipi_restart_event = ret;

		/*
		* Allocate fifos for accelerator communication
		* used both for ingress and egress.
		*/
		task_fifo_off = sbi_scratch_alloc_offset(sizeof(struct sbi_fifo));
		if (!task_fifo_off)
			return SBI_ENOMEM;

		task_fifo_mem_off = sbi_scratch_alloc_offset(sizeof(struct task_context) * 5);
		if (!task_fifo_mem_off)
			return SBI_ENOMEM;

		/* Accelerator running task state */
		task_data_off = sbi_scratch_alloc_offset(sizeof(struct task_data));
		if (!task_data_off)
			return SBI_ENOMEM;
	} else {
		if (!ipi_data_off)
			return SBI_ENOMEM;
		if (SBI_IPI_EVENT_MAX <= ipi_smode_event ||
		    SBI_IPI_EVENT_MAX <= ipi_halt_event)
			return SBI_ENOSPC;
	}

	task_fifo = sbi_scratch_offset_ptr(scratch, task_fifo_off);
	task_fifo_mem = sbi_scratch_offset_ptr(scratch, task_fifo_mem_off);
	sbi_fifo_init(task_fifo, task_fifo_mem, 5, sizeof(struct task_context));

	ipi_data = sbi_scratch_offset_ptr(scratch, ipi_data_off);
	ipi_data->ipi_type = 0x00;

	/*
	 * Initialize platform IPI support. This will also clear any
	 * pending IPIs for current/calling HART.
	 */
	ret = sbi_platform_ipi_init(sbi_platform_ptr(scratch), cold_boot);
	if (ret)
		return ret;

	if (misa_extension('V'))
		accelerator_hart = current_hartid();

	/* Enable software interrupts */
	csr_set(CSR_MIE, MIP_MSIP);

	return 0;
}

void sbi_ipi_exit(struct sbi_scratch *scratch)
{
	/* Disable software interrupts */
	csr_clear(CSR_MIE, MIP_MSIP);

	/* Process pending IPIs */
	sbi_ipi_process();

	/* Platform exit */
	sbi_platform_ipi_exit(sbi_platform_ptr(scratch));
}
