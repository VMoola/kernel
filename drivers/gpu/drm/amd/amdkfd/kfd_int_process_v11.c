/*
 * Copyright 2021 Advanced Micro Devices, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE COPYRIGHT HOLDER(S) OR AUTHOR(S) BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

#include "kfd_priv.h"
#include "kfd_events.h"
#include "soc15_int.h"
#include "kfd_device_queue_manager.h"
#include "ivsrcid/vmc/irqsrcs_vmc_1_0.h"
#include "kfd_smi_events.h"
#include "kfd_debug.h"

/*
 * GFX11 SQ Interrupts
 *
 * There are 3 encoding types of interrupts sourced from SQ sent as a 44-bit
 * packet to the Interrupt Handler:
 * Auto - Generated by the SQG (various cmd overflows, timestamps etc)
 * Wave - Generated by S_SENDMSG through a shader program
 * Error - HW generated errors (Illegal instructions, Memviols, EDC etc)
 *
 * The 44-bit packet is mapped as {context_id1[7:0],context_id0[31:0]} plus
 * 4-bits for VMID (SOC15_VMID_FROM_IH_ENTRY) as such:
 *
 * - context_id1[7:6]
 * Encoding type (0 = Auto, 1 = Wave, 2 = Error)
 *
 * - context_id0[26]
 * PRIV bit indicates that Wave S_SEND or error occurred within trap
 *
 * - context_id0[24:0]
 * 25-bit data with the following layout per encoding type:
 * Auto - only context_id0[8:0] is used, which reports various interrupts
 * generated by SQG.  The rest is 0.
 * Wave - user data sent from m0 via S_SENDMSG (context_id0[23:0])
 * Error - Error Type (context_id0[24:21]), Error Details (context_id0[20:0])
 *
 * The other context_id bits show coordinates (SE/SH/CU/SIMD/WGP) for wave
 * S_SENDMSG and Errors.  These are 0 for Auto.
 */

enum SQ_INTERRUPT_WORD_ENCODING {
	SQ_INTERRUPT_WORD_ENCODING_AUTO = 0x0,
	SQ_INTERRUPT_WORD_ENCODING_INST,
	SQ_INTERRUPT_WORD_ENCODING_ERROR,
};

enum SQ_INTERRUPT_ERROR_TYPE {
	SQ_INTERRUPT_ERROR_TYPE_EDC_FUE = 0x0,
	SQ_INTERRUPT_ERROR_TYPE_ILLEGAL_INST,
	SQ_INTERRUPT_ERROR_TYPE_MEMVIOL,
	SQ_INTERRUPT_ERROR_TYPE_EDC_FED,
};

/* SQ_INTERRUPT_WORD_AUTO_CTXID */
#define SQ_INTERRUPT_WORD_AUTO_CTXID0__THREAD_TRACE__SHIFT		0
#define SQ_INTERRUPT_WORD_AUTO_CTXID0__WLT__SHIFT			1
#define SQ_INTERRUPT_WORD_AUTO_CTXID0__THREAD_TRACE_BUF_FULL__SHIFT	2
#define SQ_INTERRUPT_WORD_AUTO_CTXID0__REG_TIMESTAMP__SHIFT		3
#define SQ_INTERRUPT_WORD_AUTO_CTXID0__CMD_TIMESTAMP__SHIFT		4
#define SQ_INTERRUPT_WORD_AUTO_CTXID0__HOST_CMD_OVERFLOW__SHIFT		5
#define SQ_INTERRUPT_WORD_AUTO_CTXID0__HOST_REG_OVERFLOW__SHIFT		6
#define SQ_INTERRUPT_WORD_AUTO_CTXID0__IMMED_OVERFLOW__SHIFT		7
#define SQ_INTERRUPT_WORD_AUTO_CTXID0__THREAD_TRACE_UTC_ERROR__SHIFT	8
#define SQ_INTERRUPT_WORD_AUTO_CTXID1__ENCODING__SHIFT			6

#define SQ_INTERRUPT_WORD_AUTO_CTXID0__THREAD_TRACE_MASK		0x00000001
#define SQ_INTERRUPT_WORD_AUTO_CTXID0__WLT_MASK				0x00000002
#define SQ_INTERRUPT_WORD_AUTO_CTXID0__THREAD_TRACE_BUF_FULL_MASK	0x00000004
#define SQ_INTERRUPT_WORD_AUTO_CTXID0__REG_TIMESTAMP_MASK		0x00000008
#define SQ_INTERRUPT_WORD_AUTO_CTXID0__CMD_TIMESTAMP_MASK		0x00000010
#define SQ_INTERRUPT_WORD_AUTO_CTXID0__HOST_CMD_OVERFLOW_MASK		0x00000020
#define SQ_INTERRUPT_WORD_AUTO_CTXID0__HOST_REG_OVERFLOW_MASK		0x00000040
#define SQ_INTERRUPT_WORD_AUTO_CTXID0__IMMED_OVERFLOW_MASK		0x00000080
#define SQ_INTERRUPT_WORD_AUTO_CTXID0__THREAD_TRACE_UTC_ERROR_MASK	0x00000100
#define SQ_INTERRUPT_WORD_AUTO_CTXID1__ENCODING_MASK			0x000000c0

/* SQ_INTERRUPT_WORD_WAVE_CTXID */
#define SQ_INTERRUPT_WORD_WAVE_CTXID0__DATA__SHIFT	0
#define SQ_INTERRUPT_WORD_WAVE_CTXID0__SH_ID__SHIFT	25
#define SQ_INTERRUPT_WORD_WAVE_CTXID0__PRIV__SHIFT	26
#define SQ_INTERRUPT_WORD_WAVE_CTXID0__WAVE_ID__SHIFT	27
#define SQ_INTERRUPT_WORD_WAVE_CTXID1__SIMD_ID__SHIFT	0
#define SQ_INTERRUPT_WORD_WAVE_CTXID1__WGP_ID__SHIFT	2
#define SQ_INTERRUPT_WORD_WAVE_CTXID1__ENCODING__SHIFT	6

#define SQ_INTERRUPT_WORD_WAVE_CTXID0__DATA_MASK	0x00ffffff /* [23:0] */
#define SQ_INTERRUPT_WORD_WAVE_CTXID0__SH_ID_MASK	0x02000000 /* [25] */
#define SQ_INTERRUPT_WORD_WAVE_CTXID0__PRIV_MASK	0x04000000 /* [26] */
#define SQ_INTERRUPT_WORD_WAVE_CTXID0__WAVE_ID_MASK	0xf8000000 /* [31:27] */
#define SQ_INTERRUPT_WORD_WAVE_CTXID1__SIMD_ID_MASK	0x00000003 /* [33:32] */
#define SQ_INTERRUPT_WORD_WAVE_CTXID1__WGP_ID_MASK	0x0000003c /* [37:34] */
#define SQ_INTERRUPT_WORD_WAVE_CTXID1__ENCODING_MASK	0x000000c0 /* [39:38] */

/* SQ_INTERRUPT_WORD_ERROR_CTXID */
#define SQ_INTERRUPT_WORD_ERROR_CTXID0__DETAIL__SHIFT	0
#define SQ_INTERRUPT_WORD_ERROR_CTXID0__TYPE__SHIFT	21
#define SQ_INTERRUPT_WORD_ERROR_CTXID0__SH_ID__SHIFT	25
#define SQ_INTERRUPT_WORD_ERROR_CTXID0__PRIV__SHIFT	26
#define SQ_INTERRUPT_WORD_ERROR_CTXID0__WAVE_ID__SHIFT	27
#define SQ_INTERRUPT_WORD_ERROR_CTXID1__SIMD_ID__SHIFT	0
#define SQ_INTERRUPT_WORD_ERROR_CTXID1__WGP_ID__SHIFT	2
#define SQ_INTERRUPT_WORD_ERROR_CTXID1__ENCODING__SHIFT	6

#define SQ_INTERRUPT_WORD_ERROR_CTXID0__DETAIL_MASK	0x001fffff /* [20:0] */
#define SQ_INTERRUPT_WORD_ERROR_CTXID0__TYPE_MASK	0x01e00000 /* [24:21] */
#define SQ_INTERRUPT_WORD_ERROR_CTXID0__SH_ID_MASK	0x02000000 /* [25] */
#define SQ_INTERRUPT_WORD_ERROR_CTXID0__PRIV_MASK	0x04000000 /* [26] */
#define SQ_INTERRUPT_WORD_ERROR_CTXID0__WAVE_ID_MASK	0xf8000000 /* [31:27] */
#define SQ_INTERRUPT_WORD_ERROR_CTXID1__SIMD_ID_MASK	0x00000003 /* [33:32] */
#define SQ_INTERRUPT_WORD_ERROR_CTXID1__WGP_ID_MASK	0x0000003c /* [37:34] */
#define SQ_INTERRUPT_WORD_ERROR_CTXID1__ENCODING_MASK	0x000000c0 /* [39:38] */

/*
 * The debugger will send user data(m0) with PRIV=1 to indicate it requires
 * notification from the KFD with the following queue id (DOORBELL_ID) and
 * trap code (TRAP_CODE).
 */
#define KFD_CTXID0_TRAP_CODE_SHIFT	10
#define KFD_CTXID0_TRAP_CODE_MASK	0xfffc00
#define KFD_CTXID0_CP_BAD_OP_ECODE_MASK	0x3ffffff
#define KFD_CTXID0_DOORBELL_ID_MASK	0x0003ff

#define KFD_CTXID0_TRAP_CODE(ctxid0)		(((ctxid0) &  \
				KFD_CTXID0_TRAP_CODE_MASK) >> \
				KFD_CTXID0_TRAP_CODE_SHIFT)
#define KFD_CTXID0_CP_BAD_OP_ECODE(ctxid0)	(((ctxid0) &        \
				KFD_CTXID0_CP_BAD_OP_ECODE_MASK) >> \
				KFD_CTXID0_TRAP_CODE_SHIFT)
#define KFD_CTXID0_DOORBELL_ID(ctxid0)		((ctxid0) & \
				KFD_CTXID0_DOORBELL_ID_MASK)

static void print_sq_intr_info_auto(uint32_t context_id0, uint32_t context_id1)
{
	pr_debug_ratelimited(
		"sq_intr: auto, ttrace %d, wlt %d, ttrace_buf_full %d, reg_tms %d, cmd_tms %d, host_cmd_ovf %d, host_reg_ovf %d, immed_ovf %d, ttrace_utc_err %d\n",
		REG_GET_FIELD(context_id0, SQ_INTERRUPT_WORD_AUTO_CTXID0, THREAD_TRACE),
		REG_GET_FIELD(context_id0, SQ_INTERRUPT_WORD_AUTO_CTXID0, WLT),
		REG_GET_FIELD(context_id0, SQ_INTERRUPT_WORD_AUTO_CTXID0, THREAD_TRACE_BUF_FULL),
		REG_GET_FIELD(context_id0, SQ_INTERRUPT_WORD_AUTO_CTXID0, REG_TIMESTAMP),
		REG_GET_FIELD(context_id0, SQ_INTERRUPT_WORD_AUTO_CTXID0, CMD_TIMESTAMP),
		REG_GET_FIELD(context_id0, SQ_INTERRUPT_WORD_AUTO_CTXID0, HOST_CMD_OVERFLOW),
		REG_GET_FIELD(context_id0, SQ_INTERRUPT_WORD_AUTO_CTXID0, HOST_REG_OVERFLOW),
		REG_GET_FIELD(context_id0, SQ_INTERRUPT_WORD_AUTO_CTXID0, IMMED_OVERFLOW),
		REG_GET_FIELD(context_id0, SQ_INTERRUPT_WORD_AUTO_CTXID0, THREAD_TRACE_UTC_ERROR));
}

static void print_sq_intr_info_inst(uint32_t context_id0, uint32_t context_id1)
{
	pr_debug_ratelimited(
		"sq_intr: inst, data 0x%08x, sh %d, priv %d, wave_id %d, simd_id %d, wgp_id %d\n",
		REG_GET_FIELD(context_id0, SQ_INTERRUPT_WORD_WAVE_CTXID0, DATA),
		REG_GET_FIELD(context_id0, SQ_INTERRUPT_WORD_WAVE_CTXID0, SH_ID),
		REG_GET_FIELD(context_id0, SQ_INTERRUPT_WORD_WAVE_CTXID0, PRIV),
		REG_GET_FIELD(context_id0, SQ_INTERRUPT_WORD_WAVE_CTXID0, WAVE_ID),
		REG_GET_FIELD(context_id1, SQ_INTERRUPT_WORD_WAVE_CTXID1, SIMD_ID),
		REG_GET_FIELD(context_id1, SQ_INTERRUPT_WORD_WAVE_CTXID1, WGP_ID));
}

static void print_sq_intr_info_error(uint32_t context_id0, uint32_t context_id1)
{
	pr_warn_ratelimited(
		"sq_intr: error, detail 0x%08x, type %d, sh %d, priv %d, wave_id %d, simd_id %d, wgp_id %d\n",
		REG_GET_FIELD(context_id0, SQ_INTERRUPT_WORD_ERROR_CTXID0, DETAIL),
		REG_GET_FIELD(context_id0, SQ_INTERRUPT_WORD_ERROR_CTXID0, TYPE),
		REG_GET_FIELD(context_id0, SQ_INTERRUPT_WORD_ERROR_CTXID0, SH_ID),
		REG_GET_FIELD(context_id0, SQ_INTERRUPT_WORD_ERROR_CTXID0, PRIV),
		REG_GET_FIELD(context_id0, SQ_INTERRUPT_WORD_ERROR_CTXID0, WAVE_ID),
		REG_GET_FIELD(context_id0, SQ_INTERRUPT_WORD_ERROR_CTXID1, SIMD_ID),
		REG_GET_FIELD(context_id0, SQ_INTERRUPT_WORD_ERROR_CTXID1, WGP_ID));
}

static void event_interrupt_poison_consumption_v11(struct kfd_node *dev,
				uint16_t pasid, uint16_t source_id)
{
	enum amdgpu_ras_block block = 0;
	int ret = -EINVAL;
	struct kfd_process *p = kfd_lookup_process_by_pasid(pasid);

	if (!p)
		return;

	/* all queues of a process will be unmapped in one time */
	if (atomic_read(&p->poison)) {
		kfd_unref_process(p);
		return;
	}

	atomic_set(&p->poison, 1);
	kfd_unref_process(p);

	switch (source_id) {
	case SOC15_INTSRC_SQ_INTERRUPT_MSG:
		if (dev->dqm->ops.reset_queues)
			ret = dev->dqm->ops.reset_queues(dev->dqm, pasid);
		block = AMDGPU_RAS_BLOCK__GFX;
		break;
	case SOC21_INTSRC_SDMA_ECC:
	default:
		block = AMDGPU_RAS_BLOCK__GFX;
		break;
	}

	kfd_signal_poison_consumed_event(dev, pasid);

	/* resetting queue passes, do page retirement without gpu reset
	   resetting queue fails, fallback to gpu reset solution */
	if (!ret)
		amdgpu_amdkfd_ras_poison_consumption_handler(dev->adev, block, false);
	else
		amdgpu_amdkfd_ras_poison_consumption_handler(dev->adev, block, true);
}

static bool event_interrupt_isr_v11(struct kfd_node *dev,
					const uint32_t *ih_ring_entry,
					uint32_t *patched_ihre,
					bool *patched_flag)
{
	uint16_t source_id, client_id, pasid, vmid;
	const uint32_t *data = ih_ring_entry;
	uint32_t context_id0;

	source_id = SOC15_SOURCE_ID_FROM_IH_ENTRY(ih_ring_entry);
	client_id = SOC15_CLIENT_ID_FROM_IH_ENTRY(ih_ring_entry);
	/* Only handle interrupts from KFD VMIDs */
	vmid = SOC15_VMID_FROM_IH_ENTRY(ih_ring_entry);
	if (!KFD_IRQ_IS_FENCE(client_id, source_id) &&
	    (vmid < dev->vm_info.first_vmid_kfd ||
	    vmid > dev->vm_info.last_vmid_kfd))
		return false;

	pasid = SOC15_PASID_FROM_IH_ENTRY(ih_ring_entry);
	context_id0 = SOC15_CONTEXT_ID0_FROM_IH_ENTRY(ih_ring_entry);

	if ((source_id == SOC15_INTSRC_CP_END_OF_PIPE) &&
	    (context_id0 & AMDGPU_FENCE_MES_QUEUE_FLAG))
		return false;

	pr_debug("client id 0x%x, source id %d, vmid %d, pasid 0x%x. raw data:\n",
		 client_id, source_id, vmid, pasid);
	pr_debug("%8X, %8X, %8X, %8X, %8X, %8X, %8X, %8X.\n",
		 data[0], data[1], data[2], data[3],
		 data[4], data[5], data[6], data[7]);

	/* If there is no valid PASID, it's likely a bug */
	if (WARN_ONCE(pasid == 0, "Bug: No PASID in KFD interrupt"))
		return false;

	/* Interrupt types we care about: various signals and faults.
	 * They will be forwarded to a work queue (see below).
	 */
	return source_id == SOC15_INTSRC_CP_END_OF_PIPE ||
		source_id == SOC15_INTSRC_SQ_INTERRUPT_MSG ||
		source_id == SOC15_INTSRC_CP_BAD_OPCODE ||
		source_id == SOC21_INTSRC_SDMA_TRAP ||
		KFD_IRQ_IS_FENCE(client_id, source_id) ||
		(((client_id == SOC21_IH_CLIENTID_VMC) ||
		 ((client_id == SOC21_IH_CLIENTID_GFX) &&
		  (source_id == UTCL2_1_0__SRCID__FAULT))) &&
		  !amdgpu_no_queue_eviction_on_vm_fault);
}

static void event_interrupt_wq_v11(struct kfd_node *dev,
					const uint32_t *ih_ring_entry)
{
	uint16_t source_id, client_id, ring_id, pasid, vmid;
	uint32_t context_id0, context_id1;
	uint8_t sq_int_enc, sq_int_priv, sq_int_errtype;
	struct kfd_vm_fault_info info = {0};
	struct kfd_hsa_memory_exception_data exception_data;

	source_id = SOC15_SOURCE_ID_FROM_IH_ENTRY(ih_ring_entry);
	client_id = SOC15_CLIENT_ID_FROM_IH_ENTRY(ih_ring_entry);
	ring_id = SOC15_RING_ID_FROM_IH_ENTRY(ih_ring_entry);
	pasid = SOC15_PASID_FROM_IH_ENTRY(ih_ring_entry);
	vmid = SOC15_VMID_FROM_IH_ENTRY(ih_ring_entry);
	context_id0 = SOC15_CONTEXT_ID0_FROM_IH_ENTRY(ih_ring_entry);
	context_id1 = SOC15_CONTEXT_ID1_FROM_IH_ENTRY(ih_ring_entry);

	/* VMC, UTCL2 */
	if (client_id == SOC21_IH_CLIENTID_VMC ||
	     ((client_id == SOC21_IH_CLIENTID_GFX) &&
	     (source_id == UTCL2_1_0__SRCID__FAULT))) {

		info.vmid = vmid;
		info.mc_id = client_id;
		info.page_addr = ih_ring_entry[4] |
			(uint64_t)(ih_ring_entry[5] & 0xf) << 32;
		info.prot_valid = ring_id & 0x08;
		info.prot_read  = ring_id & 0x10;
		info.prot_write = ring_id & 0x20;

		memset(&exception_data, 0, sizeof(exception_data));
		exception_data.gpu_id = dev->id;
		exception_data.va = (info.page_addr) << PAGE_SHIFT;
		exception_data.failure.NotPresent = info.prot_valid ? 1 : 0;
		exception_data.failure.NoExecute = info.prot_exec ? 1 : 0;
		exception_data.failure.ReadOnly = info.prot_write ? 1 : 0;
		exception_data.failure.imprecise = 0;

		kfd_set_dbg_ev_from_interrupt(dev, pasid, -1,
					      KFD_EC_MASK(EC_DEVICE_MEMORY_VIOLATION),
					      &exception_data, sizeof(exception_data));
		kfd_smi_event_update_vmfault(dev, pasid);

	/* GRBM, SDMA, SE, PMM */
	} else if (client_id == SOC21_IH_CLIENTID_GRBM_CP ||
		   client_id == SOC21_IH_CLIENTID_GFX) {

		/* CP */
		if (source_id == SOC15_INTSRC_CP_END_OF_PIPE)
			kfd_signal_event_interrupt(pasid, context_id0, 32);
		else if (source_id == SOC15_INTSRC_CP_BAD_OPCODE)
			kfd_set_dbg_ev_from_interrupt(dev, pasid,
				KFD_CTXID0_DOORBELL_ID(context_id0),
				KFD_EC_MASK(KFD_CTXID0_CP_BAD_OP_ECODE(context_id0)),
				NULL, 0);

		/* SDMA */
		else if (source_id == SOC21_INTSRC_SDMA_TRAP)
			kfd_signal_event_interrupt(pasid, context_id0 & 0xfffffff, 28);
		else if (source_id == SOC21_INTSRC_SDMA_ECC) {
			event_interrupt_poison_consumption_v11(dev, pasid, source_id);
			return;
		}

		/* SQ */
		else if (source_id == SOC15_INTSRC_SQ_INTERRUPT_MSG) {
			sq_int_enc = REG_GET_FIELD(context_id1,
					SQ_INTERRUPT_WORD_WAVE_CTXID1, ENCODING);
			switch (sq_int_enc) {
			case SQ_INTERRUPT_WORD_ENCODING_AUTO:
				print_sq_intr_info_auto(context_id0, context_id1);
				break;
			case SQ_INTERRUPT_WORD_ENCODING_INST:
				print_sq_intr_info_inst(context_id0, context_id1);
				sq_int_priv = REG_GET_FIELD(context_id0,
						SQ_INTERRUPT_WORD_WAVE_CTXID0, PRIV);
				if (sq_int_priv && (kfd_set_dbg_ev_from_interrupt(dev, pasid,
						KFD_CTXID0_DOORBELL_ID(context_id0),
						KFD_CTXID0_TRAP_CODE(context_id0),
						NULL, 0)))
					return;
				break;
			case SQ_INTERRUPT_WORD_ENCODING_ERROR:
				print_sq_intr_info_error(context_id0, context_id1);
				sq_int_errtype = REG_GET_FIELD(context_id0,
						SQ_INTERRUPT_WORD_ERROR_CTXID0, TYPE);
				if (sq_int_errtype != SQ_INTERRUPT_ERROR_TYPE_ILLEGAL_INST &&
				    sq_int_errtype != SQ_INTERRUPT_ERROR_TYPE_MEMVIOL) {
					event_interrupt_poison_consumption_v11(
							dev, pasid, source_id);
					return;
				}
				break;
			default:
				break;
			}
			kfd_signal_event_interrupt(pasid, context_id0 & 0xffffff, 24);
		}

	} else if (KFD_IRQ_IS_FENCE(client_id, source_id)) {
		kfd_process_close_interrupt_drain(pasid);
	}
}

const struct kfd_event_interrupt_class event_interrupt_class_v11 = {
	.interrupt_isr = event_interrupt_isr_v11,
	.interrupt_wq = event_interrupt_wq_v11,
};
