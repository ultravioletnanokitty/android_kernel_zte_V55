/* Copyright (c) 2012-2013, The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/delay.h>
#include "ipa_i.h"

#define IPA_HOLB_TMR_VAL 0xff

static void ipa_enable_data_path(u32 clnt_hdl)
{
	struct ipa_ep_context *ep = &ipa_ctx->ep[clnt_hdl];

	if (ipa_ctx->ipa_hw_mode == IPA_HW_MODE_VIRTUAL) {
		/* IPA_HW_MODE_VIRTUAL lacks support for TAG IC & EP suspend */
		return;
	}

	if (ipa_ctx->ipa_hw_type == IPA_HW_v1_1 && ep->suspended) {
		ipa_write_reg(ipa_ctx->mmio,
				IPA_ENDP_INIT_CTRL_n_OFST(clnt_hdl), 0);
		ep->suspended = false;
	}
}

static int ipa_disable_data_path(u32 clnt_hdl)
{
	DECLARE_COMPLETION_ONSTACK(tag_rsp);
	struct ipa_desc desc = {0};
	struct ipa_ip_packet_tag cmd;
	struct ipa_ep_context *ep = &ipa_ctx->ep[clnt_hdl];
	struct ipa_tree_node *node;
	int result = 0;

	if (ipa_ctx->ipa_hw_mode == IPA_HW_MODE_VIRTUAL) {
		/* IPA_HW_MODE_VIRTUAL lacks support for TAG IC & EP suspend */
		return 0;
	}

	node = kmem_cache_zalloc(ipa_ctx->tree_node_cache, GFP_KERNEL);
	if (!node) {
		IPAERR("failed to alloc tree node object\n");
		result = -ENOMEM;
		goto fail_alloc;
	}

	if (ipa_ctx->ipa_hw_type == IPA_HW_v1_1 && !ep->suspended) {
		ipa_write_reg(ipa_ctx->mmio,
				IPA_ENDP_INIT_CTRL_n_OFST(clnt_hdl), 1);

		cmd.tag = (u32) &tag_rsp;

		desc.pyld = &cmd;
		desc.len = sizeof(struct ipa_ip_packet_tag);
		desc.type = IPA_IMM_CMD_DESC;
		desc.opcode = IPA_IP_PACKET_TAG;

		IPADBG("Wait on TAG %p clnt=%d\n", &tag_rsp, clnt_hdl);

		node->hdl = cmd.tag;
		mutex_lock(&ipa_ctx->lock);
		if (ipa_insert(&ipa_ctx->tag_tree, node)) {
			IPAERR("failed to add to tree\n");
			result = -EINVAL;
			mutex_unlock(&ipa_ctx->lock);
			goto fail_insert;
		}
		mutex_unlock(&ipa_ctx->lock);

		if (ipa_send_cmd(1, &desc)) {
			ipa_write_reg(ipa_ctx->mmio,
				IPA_ENDP_INIT_CTRL_n_OFST(clnt_hdl), 0);
			IPAERR("fail to send TAG command\n");
			result = -EPERM;
			goto fail_send;
		}
		wait_for_completion(&tag_rsp);
		if (IPA_CLIENT_IS_CONS(ep->client) &&
				ep->cfg.aggr.aggr_en == IPA_ENABLE_AGGR &&
				ep->cfg.aggr.aggr_time_limit)
			msleep(ep->cfg.aggr.aggr_time_limit);
		ep->suspended = true;
	}

	return 0;

fail_send:
	rb_erase(&node->node, &ipa_ctx->tag_tree);
fail_insert:
	kmem_cache_free(ipa_ctx->tree_node_cache, node);
fail_alloc:
	return result;
}

static int ipa_connect_configure_sps(const struct ipa_connect_params *in,
				     struct ipa_ep_context *ep, int ipa_ep_idx)
{
	int result = -EFAULT;

	/* Default Config */
	ep->ep_hdl = sps_alloc_endpoint();

	if (ep->ep_hdl == NULL) {
		IPAERR("SPS EP alloc failed EP.\n");
		return -EFAULT;
	}

	result = sps_get_config(ep->ep_hdl,
		&ep->connect);
	if (result) {
		IPAERR("fail to get config.\n");
		return -EFAULT;
	}

	/* Specific Config */
	if (IPA_CLIENT_IS_CONS(in->client)) {
		ep->connect.mode = SPS_MODE_SRC;
		ep->connect.destination =
			in->client_bam_hdl;
		ep->connect.source = ipa_ctx->bam_handle;
		ep->connect.dest_pipe_index =
			in->client_ep_idx;
		ep->connect.src_pipe_index = ipa_ep_idx;
	} else {
		ep->connect.mode = SPS_MODE_DEST;
		ep->connect.source = in->client_bam_hdl;
		ep->connect.destination = ipa_ctx->bam_handle;
		ep->connect.src_pipe_index = in->client_ep_idx;
		ep->connect.dest_pipe_index = ipa_ep_idx;
	}

	return 0;
}

static int ipa_connect_allocate_fifo(const struct ipa_connect_params *in,
				     struct sps_mem_buffer *mem_buff_ptr,
				     bool *fifo_in_pipe_mem_ptr,
				     u32 *fifo_pipe_mem_ofst_ptr,
				     u32 fifo_size, int ipa_ep_idx)
{
	dma_addr_t dma_addr;
	u32 ofst;
	int result = -EFAULT;

	mem_buff_ptr->size = fifo_size;
	if (in->pipe_mem_preferred) {
		if (ipa_pipe_mem_alloc(&ofst, fifo_size)) {
			IPAERR("FIFO pipe mem alloc fail ep %u\n",
				ipa_ep_idx);
			mem_buff_ptr->base =
				dma_alloc_coherent(NULL,
				mem_buff_ptr->size,
				&dma_addr, GFP_KERNEL);
		} else {
			memset(mem_buff_ptr, 0, sizeof(struct sps_mem_buffer));
			result = sps_setup_bam2bam_fifo(mem_buff_ptr, ofst,
				fifo_size, 1);
			WARN_ON(result);
			*fifo_in_pipe_mem_ptr = 1;
			dma_addr = mem_buff_ptr->phys_base;
			*fifo_pipe_mem_ofst_ptr = ofst;
		}
	} else {
		mem_buff_ptr->base =
			dma_alloc_coherent(NULL, mem_buff_ptr->size,
			&dma_addr, GFP_KERNEL);
	}
	mem_buff_ptr->phys_base = dma_addr;
	if (mem_buff_ptr->base == NULL) {
		IPAERR("fail to get DMA memory.\n");
		return -EFAULT;
	}

	return 0;
}

/**
 * ipa_connect() - low-level IPA client connect
 * @in:	[in] input parameters from client
 * @sps:	[out] sps output from IPA needed by client for sps_connect
 * @clnt_hdl:	[out] opaque client handle assigned by IPA to client
 *
 * Should be called by the driver of the peripheral that wants to connect to
 * IPA in BAM-BAM mode. these peripherals are A2, USB and HSIC. this api
 * expects caller to take responsibility to add any needed headers, routing
 * and filtering tables and rules as needed.
 *
 * Returns:	0 on success, negative on failure
 *
 * Note:	Should not be called from atomic context
 */
int ipa_connect(const struct ipa_connect_params *in, struct ipa_sps_params *sps,
		u32 *clnt_hdl)
{
	int ipa_ep_idx;
	int result = -EFAULT;
	struct ipa_ep_context *ep;

	if (atomic_inc_return(&ipa_ctx->ipa_active_clients) == 1)
		if (ipa_ctx->ipa_hw_mode == IPA_HW_MODE_NORMAL)
			ipa_enable_clks();

	if (in == NULL || sps == NULL || clnt_hdl == NULL ||
	    in->client >= IPA_CLIENT_MAX ||
	    in->desc_fifo_sz == 0 || in->data_fifo_sz == 0) {
		IPAERR("bad parm.\n");
		result = -EINVAL;
		goto fail;
	}

	ipa_ep_idx = ipa_get_ep_mapping(ipa_ctx->mode, in->client);
	if (ipa_ep_idx == -1) {
		IPAERR("fail to alloc EP.\n");
		goto fail;
	}

	ep = &ipa_ctx->ep[ipa_ep_idx];

	if (ep->valid) {
		IPAERR("EP already allocated.\n");
		goto fail;
	}

	memset(&ipa_ctx->ep[ipa_ep_idx], 0, sizeof(struct ipa_ep_context));

	ep->valid = 1;
	ep->client = in->client;
	ep->client_notify = in->notify;
	ep->priv = in->priv;

	if (ipa_cfg_ep(ipa_ep_idx, &in->ipa_ep_cfg)) {
		IPAERR("fail to configure EP.\n");
		goto ipa_cfg_ep_fail;
	}

	result = ipa_connect_configure_sps(in, ep, ipa_ep_idx);
	if (result) {
		IPAERR("fail to configure SPS.\n");
		goto ipa_cfg_ep_fail;
	}

	if (in->desc.base == NULL) {
		result = ipa_connect_allocate_fifo(in, &ep->connect.desc,
						  &ep->desc_fifo_in_pipe_mem,
						  &ep->desc_fifo_pipe_mem_ofst,
						  in->desc_fifo_sz, ipa_ep_idx);
		if (result) {
			IPAERR("fail to allocate DESC FIFO.\n");
			goto desc_mem_alloc_fail;
		}
	} else {
		IPADBG("client allocated DESC FIFO\n");
		ep->connect.desc = in->desc;
		ep->desc_fifo_client_allocated = 1;
	}
	IPADBG("Descriptor FIFO pa=0x%x, size=%d\n", ep->connect.desc.phys_base,
	       ep->connect.desc.size);

	if (in->data.base == NULL) {
		result = ipa_connect_allocate_fifo(in, &ep->connect.data,
						&ep->data_fifo_in_pipe_mem,
						&ep->data_fifo_pipe_mem_ofst,
						in->data_fifo_sz, ipa_ep_idx);
		if (result) {
			IPAERR("fail to allocate DATA FIFO.\n");
			goto data_mem_alloc_fail;
		}
	} else {
		IPADBG("client allocated DATA FIFO\n");
		ep->connect.data = in->data;
		ep->data_fifo_client_allocated = 1;
	}
	IPADBG("Data FIFO pa=0x%x, size=%d\n", ep->connect.data.phys_base,
	       ep->connect.data.size);

	ep->connect.event_thresh = IPA_EVENT_THRESHOLD;
	ep->connect.options = SPS_O_AUTO_ENABLE;    /* BAM-to-BAM */

	result = sps_connect(ep->ep_hdl, &ep->connect);
	if (result) {
		IPAERR("sps_connect fails.\n");
		goto sps_connect_fail;
	}

	sps->ipa_bam_hdl = ipa_ctx->bam_handle;
	sps->ipa_ep_idx = ipa_ep_idx;
	*clnt_hdl = ipa_ep_idx;
	memcpy(&sps->desc, &ep->connect.desc, sizeof(struct sps_mem_buffer));
	memcpy(&sps->data, &ep->connect.data, sizeof(struct sps_mem_buffer));

	if (in->client == IPA_CLIENT_HSIC1_CONS ||
			in->client == IPA_CLIENT_HSIC2_CONS ||
			in->client == IPA_CLIENT_HSIC3_CONS ||
			in->client == IPA_CLIENT_HSIC4_CONS) {
		IPADBG("disable holb for ep=%d tmr=%d\n", ipa_ep_idx,
			IPA_HOLB_TMR_VAL);
		ipa_write_reg(ipa_ctx->mmio,
			IPA_ENDP_INIT_HOL_BLOCK_EN_n_OFST(ipa_ep_idx),
			0x1);
		ipa_write_reg(ipa_ctx->mmio,
			IPA_ENDP_INIT_HOL_BLOCK_TIMER_n_OFST(ipa_ep_idx),
			IPA_HOLB_TMR_VAL);
	}

	return 0;

sps_connect_fail:
	if (!ep->data_fifo_in_pipe_mem)
		dma_free_coherent(NULL,
				  ep->connect.data.size,
				  ep->connect.data.base,
				  ep->connect.data.phys_base);
	else
		ipa_pipe_mem_free(ep->data_fifo_pipe_mem_ofst,
				  ep->connect.data.size);

data_mem_alloc_fail:
	if (!ep->desc_fifo_in_pipe_mem)
		dma_free_coherent(NULL,
				  ep->connect.desc.size,
				  ep->connect.desc.base,
				  ep->connect.desc.phys_base);
	else
		ipa_pipe_mem_free(ep->desc_fifo_pipe_mem_ofst,
				  ep->connect.desc.size);

desc_mem_alloc_fail:
	sps_free_endpoint(ep->ep_hdl);
ipa_cfg_ep_fail:
	memset(&ipa_ctx->ep[ipa_ep_idx], 0, sizeof(struct ipa_ep_context));
fail:
	if (atomic_dec_return(&ipa_ctx->ipa_active_clients) == 0) {
		if (ipa_ctx->ipa_hw_mode == IPA_HW_MODE_NORMAL)
			ipa_disable_clks();
	}

	return result;
}
EXPORT_SYMBOL(ipa_connect);
/**
 * ipa_disconnect() - low-level IPA client disconnect
 * @clnt_hdl:	[in] opaque client handle assigned by IPA to client
 *
 * Should be called by the driver of the peripheral that wants to disconnect
 * from IPA in BAM-BAM mode. this api expects caller to take responsibility to
 * free any needed headers, routing and filtering tables and rules as needed.
 *
 * Returns:	0 on success, negative on failure
 *
 * Note:	Should not be called from atomic context
 */
int ipa_disconnect(u32 clnt_hdl)
{
	int result;
	struct ipa_ep_context *ep;

	if (clnt_hdl >= IPA_NUM_PIPES || ipa_ctx->ep[clnt_hdl].valid == 0) {
		IPAERR("bad parm.\n");
		return -EINVAL;
	}

	ep = &ipa_ctx->ep[clnt_hdl];

	result = ipa_disable_data_path(clnt_hdl);
	if (result) {
		IPAERR("disable data path failed res=%d clnt=%d.\n", result,
				clnt_hdl);
		return -EPERM;
	}

	result = sps_disconnect(ep->ep_hdl);
	if (result) {
		IPAERR("SPS disconnect failed.\n");
		return -EPERM;
	}

	if (!ep->desc_fifo_client_allocated &&
	     ep->connect.desc.base) {
		if (!ep->desc_fifo_in_pipe_mem)
			dma_free_coherent(NULL,
					  ep->connect.desc.size,
					  ep->connect.desc.base,
					  ep->connect.desc.phys_base);
		else
			ipa_pipe_mem_free(ep->desc_fifo_pipe_mem_ofst,
					  ep->connect.desc.size);
	}

	if (!ep->data_fifo_client_allocated &&
	     ep->connect.data.base) {
		if (!ep->data_fifo_in_pipe_mem)
			dma_free_coherent(NULL,
					  ep->connect.data.size,
					  ep->connect.data.base,
					  ep->connect.data.phys_base);
		else
			ipa_pipe_mem_free(ep->data_fifo_pipe_mem_ofst,
					  ep->connect.data.size);
	}

	result = sps_free_endpoint(ep->ep_hdl);
	if (result) {
		IPAERR("SPS de-alloc EP failed.\n");
		return -EPERM;
	}

	ipa_enable_data_path(clnt_hdl);
	memset(&ipa_ctx->ep[clnt_hdl], 0, sizeof(struct ipa_ep_context));

	if (atomic_dec_return(&ipa_ctx->ipa_active_clients) == 0) {
		if (ipa_ctx->ipa_hw_mode == IPA_HW_MODE_NORMAL)
			ipa_disable_clks();
	}

	return 0;
}
EXPORT_SYMBOL(ipa_disconnect);

/**
 * ipa_connection_suspend() - suspend B2B connection to/from IPA
 * @clnt_hdl:	[in] opaque client handle assigned by IPA to client
 *
 * Should be called by the driver of the peripheral that wants to suspend
 * its BAM-BAM connection to/from IPA in BAM-BAM mode. The pipe is not
 * disconnected and must later be resumed before data transfer can begin
 *
 * Returns:	0 on success, negative on failure
 *
 * Note:	Should not be called from atomic context
 */
int ipa_connection_suspend(u32 clnt_hdl)
{
	int result;

	if (clnt_hdl >= IPA_NUM_PIPES || ipa_ctx->ep[clnt_hdl].valid == 0) {
		IPAERR("bad parm.\n");
		return -EINVAL;
	}
	result = ipa_disable_data_path(clnt_hdl);
	if (result)
		IPAERR("disable data path failed res=%d clnt=%d.\n", result,
				clnt_hdl);

	return result;
}
EXPORT_SYMBOL(ipa_connection_suspend);

/**
 * ipa_connection_resume() - resume B2B connection to/from IPA
 * @clnt_hdl:	[in] opaque client handle assigned by IPA to client
 *
 * Should be called by the driver of the peripheral that wants to resume
 * its previously suspended BAM-BAM connection to/from IPA in BAM-BAM mode.
 *
 * Returns:	0 on success, negative on failure
 *
 * Note:	Should not be called from atomic context
 */
int ipa_connection_resume(u32 clnt_hdl)
{
	if (clnt_hdl >= IPA_NUM_PIPES || ipa_ctx->ep[clnt_hdl].valid == 0) {
		IPAERR("bad parm.\n");
		return -EINVAL;
	}

	ipa_enable_data_path(clnt_hdl);

	return 0;
}
EXPORT_SYMBOL(ipa_connection_resume);

