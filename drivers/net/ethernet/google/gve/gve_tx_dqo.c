// SPDX-License-Identifier: (GPL-2.0 OR MIT)
/* Google virtual Ethernet (gve) driver
 *
 * Copyright (C) 2015-2021 Google, Inc.
 */

#include "gve.h"
#include "gve_adminq.h"
#include "gve_utils.h"
#include "gve_dqo.h"
#include <net/ip.h>
#include <linux/tcp.h>
#include <linux/slab.h>
#include <linux/skbuff.h>

/* Returns true if tx_bufs are available. */
static bool gve_has_free_tx_qpl_bufs(struct gve_tx_ring *tx, int count)
{
	int num_avail;

	if (!tx->dqo.qpl)
		return true;

	num_avail = tx->dqo.num_tx_qpl_bufs -
		(tx->dqo_tx.alloc_tx_qpl_buf_cnt -
		 tx->dqo_tx.free_tx_qpl_buf_cnt);

	if (count <= num_avail)
		return true;

	/* Update cached value from dqo_compl. */
	tx->dqo_tx.free_tx_qpl_buf_cnt =
		atomic_read_acquire(&tx->dqo_compl.free_tx_qpl_buf_cnt);

	num_avail = tx->dqo.num_tx_qpl_bufs -
		(tx->dqo_tx.alloc_tx_qpl_buf_cnt -
		 tx->dqo_tx.free_tx_qpl_buf_cnt);

	return count <= num_avail;
}

static s16
gve_alloc_tx_qpl_buf(struct gve_tx_ring *tx)
{
	s16 index;

	index = tx->dqo_tx.free_tx_qpl_buf_head;

	/* No TX buffers available, try to steal the list from the
	 * completion handler.
	 */
	if (unlikely(index == -1)) {
		tx->dqo_tx.free_tx_qpl_buf_head =
			atomic_xchg(&tx->dqo_compl.free_tx_qpl_buf_head, -1);
		index = tx->dqo_tx.free_tx_qpl_buf_head;

		if (unlikely(index == -1))
			return index;
	}

	/* Remove TX buf from free list */
	tx->dqo_tx.free_tx_qpl_buf_head = tx->dqo.tx_qpl_buf_next[index];

	return index;
}

static void
gve_free_tx_qpl_bufs(struct gve_tx_ring *tx,
		     struct gve_tx_pending_packet_dqo *pkt)
{
	s16 index;
	int i;

	if (!pkt->num_bufs)
		return;

	index = pkt->tx_qpl_buf_ids[0];
	/* Create a linked list of buffers to be added to the free list */
	for (i = 1; i < pkt->num_bufs; i++) {
		tx->dqo.tx_qpl_buf_next[index] = pkt->tx_qpl_buf_ids[i];
		index = pkt->tx_qpl_buf_ids[i];
	}

	while (true) {
		s16 old_head = atomic_read_acquire(&tx->dqo_compl.free_tx_qpl_buf_head);

		tx->dqo.tx_qpl_buf_next[index] = old_head;
		if (atomic_cmpxchg(&tx->dqo_compl.free_tx_qpl_buf_head,
				   old_head,
				   pkt->tx_qpl_buf_ids[0]) == old_head) {
			break;
		}
	}

	atomic_add(pkt->num_bufs, &tx->dqo_compl.free_tx_qpl_buf_cnt);
	pkt->num_bufs = 0;
}

/* Returns true if a gve_tx_pending_packet_dqo object is available. */
static bool gve_has_pending_packet(struct gve_tx_ring *tx)
{
	/* Check TX path's list. */
	if (tx->dqo_tx.free_pending_packets != -1)
		return true;

	/* Check completion handler's list. */
	if (atomic_read_acquire(&tx->dqo_compl.free_pending_packets) != -1)
		return true;

	return false;
}

static struct gve_tx_pending_packet_dqo *
gve_alloc_pending_packet(struct gve_tx_ring *tx)
{
	struct gve_tx_pending_packet_dqo *pending_packet;
	s16 index;

	index = tx->dqo_tx.free_pending_packets;

	/* No pending_packets available, try to steal the list from the
	 * completion handler.
	 */
	if (unlikely(index == -1)) {
		tx->dqo_tx.free_pending_packets =
			atomic_xchg(&tx->dqo_compl.free_pending_packets, -1);
		index = tx->dqo_tx.free_pending_packets;

		if (unlikely(index == -1))
			return NULL;
	}

	pending_packet = &tx->dqo.pending_packets[index];

	/* Remove pending_packet from free list */
	tx->dqo_tx.free_pending_packets = pending_packet->next;
	pending_packet->state = GVE_PACKET_STATE_PENDING_DATA_COMPL;

	return pending_packet;
}

static void
gve_free_pending_packet(struct gve_tx_ring *tx,
			struct gve_tx_pending_packet_dqo *pending_packet)
{
	s16 index = pending_packet - tx->dqo.pending_packets;

	pending_packet->state = GVE_PACKET_STATE_UNALLOCATED;
	while (true) {
		s16 old_head = atomic_read_acquire(&tx->dqo_compl.free_pending_packets);

		pending_packet->next = old_head;
		if (atomic_cmpxchg(&tx->dqo_compl.free_pending_packets,
				   old_head, index) == old_head) {
			break;
		}
	}
}

/* gve_tx_free_desc - Cleans up all pending tx requests and buffers.
 */
static void gve_tx_clean_pending_packets(struct gve_tx_ring *tx)
{
	int i;

	for (i = 0; i < tx->dqo.num_pending_packets; i++) {
		struct gve_tx_pending_packet_dqo *cur_state =
			&tx->dqo.pending_packets[i];
		int j;

		for (j = 0; j < cur_state->num_bufs; j++) {
			if (j == 0) {
				dma_unmap_single(tx->dev,
					dma_unmap_addr(cur_state, dma[j]),
					dma_unmap_len(cur_state, len[j]),
					DMA_TO_DEVICE);
			} else {
				dma_unmap_page(tx->dev,
					dma_unmap_addr(cur_state, dma[j]),
					dma_unmap_len(cur_state, len[j]),
					DMA_TO_DEVICE);
			}
		}
		if (cur_state->skb) {
			dev_consume_skb_any(cur_state->skb);
			cur_state->skb = NULL;
		}
	}
}

void gve_tx_stop_ring_dqo(struct gve_priv *priv, int idx)
{
	int ntfy_idx = gve_tx_idx_to_ntfy(priv, idx);
	struct gve_tx_ring *tx = &priv->tx[idx];

	if (!gve_tx_was_added_to_block(priv, idx))
		return;

	gve_remove_napi(priv, ntfy_idx);
	gve_clean_tx_done_dqo(priv, tx, /*napi=*/NULL);
	netdev_tx_reset_queue(tx->netdev_txq);
	gve_tx_clean_pending_packets(tx);
	gve_tx_remove_from_block(priv, idx);
}

static void gve_tx_free_ring_dqo(struct gve_priv *priv, struct gve_tx_ring *tx,
				 struct gve_tx_alloc_rings_cfg *cfg)
{
	struct device *hdev = &priv->pdev->dev;
	int idx = tx->q_num;
	size_t bytes;
	u32 qpl_id;

	if (tx->q_resources) {
		dma_free_coherent(hdev, sizeof(*tx->q_resources),
				  tx->q_resources, tx->q_resources_bus);
		tx->q_resources = NULL;
	}

	if (tx->dqo.compl_ring) {
		bytes = sizeof(tx->dqo.compl_ring[0]) *
			(tx->dqo.complq_mask + 1);
		dma_free_coherent(hdev, bytes, tx->dqo.compl_ring,
				  tx->complq_bus_dqo);
		tx->dqo.compl_ring = NULL;
	}

	if (tx->dqo.tx_ring) {
		bytes = sizeof(tx->dqo.tx_ring[0]) * (tx->mask + 1);
		dma_free_coherent(hdev, bytes, tx->dqo.tx_ring, tx->bus);
		tx->dqo.tx_ring = NULL;
	}

	kvfree(tx->dqo.pending_packets);
	tx->dqo.pending_packets = NULL;

	kvfree(tx->dqo.tx_qpl_buf_next);
	tx->dqo.tx_qpl_buf_next = NULL;

	if (tx->dqo.qpl) {
		qpl_id = gve_tx_qpl_id(priv, tx->q_num);
		gve_free_queue_page_list(priv, tx->dqo.qpl, qpl_id);
		tx->dqo.qpl = NULL;
	}

	netif_dbg(priv, drv, priv->dev, "freed tx queue %d\n", idx);
}

static int gve_tx_qpl_buf_init(struct gve_tx_ring *tx)
{
	int num_tx_qpl_bufs = GVE_TX_BUFS_PER_PAGE_DQO *
		tx->dqo.qpl->num_entries;
	int i;

	tx->dqo.tx_qpl_buf_next = kvcalloc(num_tx_qpl_bufs,
					   sizeof(tx->dqo.tx_qpl_buf_next[0]),
					   GFP_KERNEL);
	if (!tx->dqo.tx_qpl_buf_next)
		return -ENOMEM;

	tx->dqo.num_tx_qpl_bufs = num_tx_qpl_bufs;

	/* Generate free TX buf list */
	for (i = 0; i < num_tx_qpl_bufs - 1; i++)
		tx->dqo.tx_qpl_buf_next[i] = i + 1;
	tx->dqo.tx_qpl_buf_next[num_tx_qpl_bufs - 1] = -1;

	atomic_set_release(&tx->dqo_compl.free_tx_qpl_buf_head, -1);
	return 0;
}

void gve_tx_start_ring_dqo(struct gve_priv *priv, int idx)
{
	int ntfy_idx = gve_tx_idx_to_ntfy(priv, idx);
	struct gve_tx_ring *tx = &priv->tx[idx];

	gve_tx_add_to_block(priv, idx);

	tx->netdev_txq = netdev_get_tx_queue(priv->dev, idx);
	gve_add_napi(priv, ntfy_idx, gve_napi_poll_dqo);
}

static int gve_tx_alloc_ring_dqo(struct gve_priv *priv,
				 struct gve_tx_alloc_rings_cfg *cfg,
				 struct gve_tx_ring *tx,
				 int idx)
{
	struct device *hdev = &priv->pdev->dev;
	int num_pending_packets;
	int qpl_page_cnt;
	size_t bytes;
	u32 qpl_id;
	int i;

	memset(tx, 0, sizeof(*tx));
	tx->q_num = idx;
	tx->dev = hdev;
	atomic_set_release(&tx->dqo_compl.hw_tx_head, 0);

	/* Queue sizes must be a power of 2 */
	tx->mask = cfg->ring_size - 1;
	tx->dqo.complq_mask = tx->mask;

	/* The max number of pending packets determines the maximum number of
	 * descriptors which maybe written to the completion queue.
	 *
	 * We must set the number small enough to make sure we never overrun the
	 * completion queue.
	 */
	num_pending_packets = tx->dqo.complq_mask + 1;

	/* Reserve space for descriptor completions, which will be reported at
	 * most every GVE_TX_MIN_RE_INTERVAL packets.
	 */
	num_pending_packets -=
		(tx->dqo.complq_mask + 1) / GVE_TX_MIN_RE_INTERVAL;

	/* Each packet may have at most 2 buffer completions if it receives both
	 * a miss and reinjection completion.
	 */
	num_pending_packets /= 2;

	tx->dqo.num_pending_packets = min_t(int, num_pending_packets, S16_MAX);
	tx->dqo.pending_packets = kvcalloc(tx->dqo.num_pending_packets,
					   sizeof(tx->dqo.pending_packets[0]),
					   GFP_KERNEL);
	if (!tx->dqo.pending_packets)
		goto err;

	/* Set up linked list of pending packets */
	for (i = 0; i < tx->dqo.num_pending_packets - 1; i++)
		tx->dqo.pending_packets[i].next = i + 1;

	tx->dqo.pending_packets[tx->dqo.num_pending_packets - 1].next = -1;
	atomic_set_release(&tx->dqo_compl.free_pending_packets, -1);
	tx->dqo_compl.miss_completions.head = -1;
	tx->dqo_compl.miss_completions.tail = -1;
	tx->dqo_compl.timed_out_completions.head = -1;
	tx->dqo_compl.timed_out_completions.tail = -1;

	bytes = sizeof(tx->dqo.tx_ring[0]) * (tx->mask + 1);
	tx->dqo.tx_ring = dma_alloc_coherent(hdev, bytes, &tx->bus, GFP_KERNEL);
	if (!tx->dqo.tx_ring)
		goto err;

	bytes = sizeof(tx->dqo.compl_ring[0]) * (tx->dqo.complq_mask + 1);
	tx->dqo.compl_ring = dma_alloc_coherent(hdev, bytes,
						&tx->complq_bus_dqo,
						GFP_KERNEL);
	if (!tx->dqo.compl_ring)
		goto err;

	tx->q_resources = dma_alloc_coherent(hdev, sizeof(*tx->q_resources),
					     &tx->q_resources_bus, GFP_KERNEL);
	if (!tx->q_resources)
		goto err;

	if (!cfg->raw_addressing) {
		qpl_id = gve_tx_qpl_id(priv, tx->q_num);
		qpl_page_cnt = priv->tx_pages_per_qpl;

		tx->dqo.qpl = gve_alloc_queue_page_list(priv, qpl_id,
							qpl_page_cnt);
		if (!tx->dqo.qpl)
			goto err;

		if (gve_tx_qpl_buf_init(tx))
			goto err;
	}

	return 0;

err:
	gve_tx_free_ring_dqo(priv, tx, cfg);
	return -ENOMEM;
}

int gve_tx_alloc_rings_dqo(struct gve_priv *priv,
			   struct gve_tx_alloc_rings_cfg *cfg)
{
	struct gve_tx_ring *tx = cfg->tx;
	int err = 0;
	int i, j;

	if (cfg->start_idx + cfg->num_rings > cfg->qcfg->max_queues) {
		netif_err(priv, drv, priv->dev,
			  "Cannot alloc more than the max num of Tx rings\n");
		return -EINVAL;
	}

	if (cfg->start_idx == 0) {
		tx = kvcalloc(cfg->qcfg->max_queues, sizeof(struct gve_tx_ring),
			      GFP_KERNEL);
		if (!tx)
			return -ENOMEM;
	} else if (!tx) {
		netif_err(priv, drv, priv->dev,
			  "Cannot alloc tx rings from a nonzero start idx without tx array\n");
		return -EINVAL;
	}

	for (i = cfg->start_idx; i < cfg->start_idx + cfg->num_rings; i++) {
		err = gve_tx_alloc_ring_dqo(priv, cfg, &tx[i], i);
		if (err) {
			netif_err(priv, drv, priv->dev,
				  "Failed to alloc tx ring=%d: err=%d\n",
				  i, err);
			goto err;
		}
	}

	cfg->tx = tx;
	return 0;

err:
	for (j = 0; j < i; j++)
		gve_tx_free_ring_dqo(priv, &tx[j], cfg);
	if (cfg->start_idx == 0)
		kvfree(tx);
	return err;
}

void gve_tx_free_rings_dqo(struct gve_priv *priv,
			   struct gve_tx_alloc_rings_cfg *cfg)
{
	struct gve_tx_ring *tx = cfg->tx;
	int i;

	if (!tx)
		return;

	for (i = cfg->start_idx; i < cfg->start_idx + cfg->num_rings; i++)
		gve_tx_free_ring_dqo(priv, &tx[i], cfg);

	if (cfg->start_idx == 0) {
		kvfree(tx);
		cfg->tx = NULL;
	}
}

/* Returns the number of slots available in the ring */
static u32 num_avail_tx_slots(const struct gve_tx_ring *tx)
{
	u32 num_used = (tx->dqo_tx.tail - tx->dqo_tx.head) & tx->mask;

	return tx->mask - num_used;
}

static bool gve_has_avail_slots_tx_dqo(struct gve_tx_ring *tx,
				       int desc_count, int buf_count)
{
	return gve_has_pending_packet(tx) &&
		   num_avail_tx_slots(tx) >= desc_count &&
		   gve_has_free_tx_qpl_bufs(tx, buf_count);
}

/* Stops the queue if available descriptors is less than 'count'.
 * Return: 0 if stop is not required.
 */
static int gve_maybe_stop_tx_dqo(struct gve_tx_ring *tx,
				 int desc_count, int buf_count)
{
	if (likely(gve_has_avail_slots_tx_dqo(tx, desc_count, buf_count)))
		return 0;

	/* Update cached TX head pointer */
	tx->dqo_tx.head = atomic_read_acquire(&tx->dqo_compl.hw_tx_head);

	if (likely(gve_has_avail_slots_tx_dqo(tx, desc_count, buf_count)))
		return 0;

	/* No space, so stop the queue */
	tx->stop_queue++;
	netif_tx_stop_queue(tx->netdev_txq);

	/* Sync with restarting queue in `gve_tx_poll_dqo()` */
	mb();

	/* After stopping queue, check if we can transmit again in order to
	 * avoid TOCTOU bug.
	 */
	tx->dqo_tx.head = atomic_read_acquire(&tx->dqo_compl.hw_tx_head);

	if (likely(!gve_has_avail_slots_tx_dqo(tx, desc_count, buf_count)))
		return -EBUSY;

	netif_tx_start_queue(tx->netdev_txq);
	tx->wake_queue++;
	return 0;
}

static void gve_extract_tx_metadata_dqo(const struct sk_buff *skb,
					struct gve_tx_metadata_dqo *metadata)
{
	memset(metadata, 0, sizeof(*metadata));
	metadata->version = GVE_TX_METADATA_VERSION_DQO;

	if (skb->l4_hash) {
		u16 path_hash = skb->hash ^ (skb->hash >> 16);

		path_hash &= (1 << 15) - 1;
		if (unlikely(path_hash == 0))
			path_hash = ~path_hash;

		metadata->path_hash = path_hash;
	}
}

static void gve_tx_fill_pkt_desc_dqo(struct gve_tx_ring *tx, u32 *desc_idx,
				     struct sk_buff *skb, u32 len, u64 addr,
				     s16 compl_tag, bool eop, bool is_gso)
{
	const bool checksum_offload_en = skb->ip_summed == CHECKSUM_PARTIAL;

	while (len > 0) {
		struct gve_tx_pkt_desc_dqo *desc =
			&tx->dqo.tx_ring[*desc_idx].pkt;
		u32 cur_len = min_t(u32, len, GVE_TX_MAX_BUF_SIZE_DQO);
		bool cur_eop = eop && cur_len == len;

		*desc = (struct gve_tx_pkt_desc_dqo){
			.buf_addr = cpu_to_le64(addr),
			.dtype = GVE_TX_PKT_DESC_DTYPE_DQO,
			.end_of_packet = cur_eop,
			.checksum_offload_enable = checksum_offload_en,
			.compl_tag = cpu_to_le16(compl_tag),
			.buf_size = cur_len,
		};

		addr += cur_len;
		len -= cur_len;
		*desc_idx = (*desc_idx + 1) & tx->mask;
	}
}

/* Validates and prepares `skb` for TSO.
 *
 * Returns header length, or < 0 if invalid.
 */
static int gve_prep_tso(struct sk_buff *skb)
{
	struct tcphdr *tcp;
	int header_len;
	u32 paylen;
	int err;

	/* Note: HW requires MSS (gso_size) to be <= 9728 and the total length
	 * of the TSO to be <= 262143.
	 *
	 * However, we don't validate these because:
	 * - Hypervisor enforces a limit of 9K MTU
	 * - Kernel will not produce a TSO larger than 64k
	 */

	if (unlikely(skb_shinfo(skb)->gso_size < GVE_TX_MIN_TSO_MSS_DQO))
		return -1;

	if (!(skb_shinfo(skb)->gso_type & (SKB_GSO_TCPV4 | SKB_GSO_TCPV6)))
		return -EINVAL;

	/* Needed because we will modify header. */
	err = skb_cow_head(skb, 0);
	if (err < 0)
		return err;

	tcp = tcp_hdr(skb);
	paylen = skb->len - skb_transport_offset(skb);
	csum_replace_by_diff(&tcp->check, (__force __wsum)htonl(paylen));
	header_len = skb_tcp_all_headers(skb);

	if (unlikely(header_len > GVE_TX_MAX_HDR_SIZE_DQO))
		return -EINVAL;

	return header_len;
}

static void gve_tx_fill_tso_ctx_desc(struct gve_tx_tso_context_desc_dqo *desc,
				     const struct sk_buff *skb,
				     const struct gve_tx_metadata_dqo *metadata,
				     int header_len)
{
	*desc = (struct gve_tx_tso_context_desc_dqo){
		.header_len = header_len,
		.cmd_dtype = {
			.dtype = GVE_TX_TSO_CTX_DESC_DTYPE_DQO,
			.tso = 1,
		},
		.flex0 = metadata->bytes[0],
		.flex5 = metadata->bytes[5],
		.flex6 = metadata->bytes[6],
		.flex7 = metadata->bytes[7],
		.flex8 = metadata->bytes[8],
		.flex9 = metadata->bytes[9],
		.flex10 = metadata->bytes[10],
		.flex11 = metadata->bytes[11],
	};
	desc->tso_total_len = skb->len - header_len;
	desc->mss = skb_shinfo(skb)->gso_size;
}

static void
gve_tx_fill_general_ctx_desc(struct gve_tx_general_context_desc_dqo *desc,
			     const struct gve_tx_metadata_dqo *metadata)
{
	*desc = (struct gve_tx_general_context_desc_dqo){
		.flex0 = metadata->bytes[0],
		.flex1 = metadata->bytes[1],
		.flex2 = metadata->bytes[2],
		.flex3 = metadata->bytes[3],
		.flex4 = metadata->bytes[4],
		.flex5 = metadata->bytes[5],
		.flex6 = metadata->bytes[6],
		.flex7 = metadata->bytes[7],
		.flex8 = metadata->bytes[8],
		.flex9 = metadata->bytes[9],
		.flex10 = metadata->bytes[10],
		.flex11 = metadata->bytes[11],
		.cmd_dtype = {.dtype = GVE_TX_GENERAL_CTX_DESC_DTYPE_DQO},
	};
}

static int gve_tx_add_skb_no_copy_dqo(struct gve_tx_ring *tx,
				      struct sk_buff *skb,
				      struct gve_tx_pending_packet_dqo *pkt,
				      s16 completion_tag,
				      u32 *desc_idx,
				      bool is_gso)
{
	const struct skb_shared_info *shinfo = skb_shinfo(skb);
	int i;

	/* Note: HW requires that the size of a non-TSO packet be within the
	 * range of [17, 9728].
	 *
	 * We don't double check because
	 * - We limited `netdev->min_mtu` to ETH_MIN_MTU.
	 * - Hypervisor won't allow MTU larger than 9216.
	 */

	pkt->num_bufs = 0;
	/* Map the linear portion of skb */
	{
		u32 len = skb_headlen(skb);
		dma_addr_t addr;

		addr = dma_map_single(tx->dev, skb->data, len, DMA_TO_DEVICE);
		if (unlikely(dma_mapping_error(tx->dev, addr)))
			goto err;

		dma_unmap_len_set(pkt, len[pkt->num_bufs], len);
		dma_unmap_addr_set(pkt, dma[pkt->num_bufs], addr);
		++pkt->num_bufs;

		gve_tx_fill_pkt_desc_dqo(tx, desc_idx, skb, len, addr,
					 completion_tag,
					 /*eop=*/shinfo->nr_frags == 0, is_gso);
	}

	for (i = 0; i < shinfo->nr_frags; i++) {
		const skb_frag_t *frag = &shinfo->frags[i];
		bool is_eop = i == (shinfo->nr_frags - 1);
		u32 len = skb_frag_size(frag);
		dma_addr_t addr;

		addr = skb_frag_dma_map(tx->dev, frag, 0, len, DMA_TO_DEVICE);
		if (unlikely(dma_mapping_error(tx->dev, addr)))
			goto err;

		dma_unmap_len_set(pkt, len[pkt->num_bufs], len);
		dma_unmap_addr_set(pkt, dma[pkt->num_bufs], addr);
		++pkt->num_bufs;

		gve_tx_fill_pkt_desc_dqo(tx, desc_idx, skb, len, addr,
					 completion_tag, is_eop, is_gso);
	}

	return 0;
err:
	for (i = 0; i < pkt->num_bufs; i++) {
		if (i == 0) {
			dma_unmap_single(tx->dev,
					 dma_unmap_addr(pkt, dma[i]),
					 dma_unmap_len(pkt, len[i]),
					 DMA_TO_DEVICE);
		} else {
			dma_unmap_page(tx->dev,
				       dma_unmap_addr(pkt, dma[i]),
				       dma_unmap_len(pkt, len[i]),
				       DMA_TO_DEVICE);
		}
	}
	pkt->num_bufs = 0;
	return -1;
}

/* Tx buffer i corresponds to
 * qpl_page_id = i / GVE_TX_BUFS_PER_PAGE_DQO
 * qpl_page_offset = (i % GVE_TX_BUFS_PER_PAGE_DQO) * GVE_TX_BUF_SIZE_DQO
 */
static void gve_tx_buf_get_addr(struct gve_tx_ring *tx,
				s16 index,
				void **va, dma_addr_t *dma_addr)
{
	int page_id = index >> (PAGE_SHIFT - GVE_TX_BUF_SHIFT_DQO);
	int offset = (index & (GVE_TX_BUFS_PER_PAGE_DQO - 1)) << GVE_TX_BUF_SHIFT_DQO;

	*va = page_address(tx->dqo.qpl->pages[page_id]) + offset;
	*dma_addr = tx->dqo.qpl->page_buses[page_id] + offset;
}

static int gve_tx_add_skb_copy_dqo(struct gve_tx_ring *tx,
				   struct sk_buff *skb,
				   struct gve_tx_pending_packet_dqo *pkt,
				   s16 completion_tag,
				   u32 *desc_idx,
				   bool is_gso)
{
	u32 copy_offset = 0;
	dma_addr_t dma_addr;
	u32 copy_len;
	s16 index;
	void *va;

	/* Break the packet into buffer size chunks */
	pkt->num_bufs = 0;
	while (copy_offset < skb->len) {
		index = gve_alloc_tx_qpl_buf(tx);
		if (unlikely(index == -1))
			goto err;

		gve_tx_buf_get_addr(tx, index, &va, &dma_addr);
		copy_len = min_t(u32, GVE_TX_BUF_SIZE_DQO,
				 skb->len - copy_offset);
		skb_copy_bits(skb, copy_offset, va, copy_len);

		copy_offset += copy_len;
		dma_sync_single_for_device(tx->dev, dma_addr,
					   copy_len, DMA_TO_DEVICE);
		gve_tx_fill_pkt_desc_dqo(tx, desc_idx, skb,
					 copy_len,
					 dma_addr,
					 completion_tag,
					 copy_offset == skb->len,
					 is_gso);

		pkt->tx_qpl_buf_ids[pkt->num_bufs] = index;
		++tx->dqo_tx.alloc_tx_qpl_buf_cnt;
		++pkt->num_bufs;
	}

	return 0;
err:
	/* Should not be here if gve_has_free_tx_qpl_bufs() check is correct */
	gve_free_tx_qpl_bufs(tx, pkt);
	return -ENOMEM;
}

/* Returns 0 on success, or < 0 on error.
 *
 * Before this function is called, the caller must ensure
 * gve_has_pending_packet(tx) returns true.
 */
static int gve_tx_add_skb_dqo(struct gve_tx_ring *tx,
			      struct sk_buff *skb)
{
	const bool is_gso = skb_is_gso(skb);
	u32 desc_idx = tx->dqo_tx.tail;
	struct gve_tx_pending_packet_dqo *pkt;
	struct gve_tx_metadata_dqo metadata;
	s16 completion_tag;

	pkt = gve_alloc_pending_packet(tx);
	if (!pkt)
		return -ENOMEM;

	pkt->skb = skb;
	completion_tag = pkt - tx->dqo.pending_packets;

	gve_extract_tx_metadata_dqo(skb, &metadata);
	if (is_gso) {
		int header_len = gve_prep_tso(skb);

		if (unlikely(header_len < 0))
			goto err;

		gve_tx_fill_tso_ctx_desc(&tx->dqo.tx_ring[desc_idx].tso_ctx,
					 skb, &metadata, header_len);
		desc_idx = (desc_idx + 1) & tx->mask;
	}

	gve_tx_fill_general_ctx_desc(&tx->dqo.tx_ring[desc_idx].general_ctx,
				     &metadata);
	desc_idx = (desc_idx + 1) & tx->mask;

	if (tx->dqo.qpl) {
		if (gve_tx_add_skb_copy_dqo(tx, skb, pkt,
					    completion_tag,
					    &desc_idx, is_gso))
			goto err;
	}  else {
		if (gve_tx_add_skb_no_copy_dqo(tx, skb, pkt,
					       completion_tag,
					       &desc_idx, is_gso))
			goto err;
	}

	tx->dqo_tx.posted_packet_desc_cnt += pkt->num_bufs;

	/* Commit the changes to our state */
	tx->dqo_tx.tail = desc_idx;

	/* Request a descriptor completion on the last descriptor of the
	 * packet if we are allowed to by the HW enforced interval.
	 */
	{
		u32 last_desc_idx = (desc_idx - 1) & tx->mask;
		u32 last_report_event_interval =
			(last_desc_idx - tx->dqo_tx.last_re_idx) & tx->mask;

		if (unlikely(last_report_event_interval >=
			     GVE_TX_MIN_RE_INTERVAL)) {
			tx->dqo.tx_ring[last_desc_idx].pkt.report_event = true;
			tx->dqo_tx.last_re_idx = last_desc_idx;
		}
	}

	return 0;

err:
	pkt->skb = NULL;
	gve_free_pending_packet(tx, pkt);

	return -1;
}

static int gve_num_descs_per_buf(size_t size)
{
	return DIV_ROUND_UP(size, GVE_TX_MAX_BUF_SIZE_DQO);
}

static int gve_num_buffer_descs_needed(const struct sk_buff *skb)
{
	const struct skb_shared_info *shinfo = skb_shinfo(skb);
	int num_descs;
	int i;

	num_descs = gve_num_descs_per_buf(skb_headlen(skb));

	for (i = 0; i < shinfo->nr_frags; i++) {
		unsigned int frag_size = skb_frag_size(&shinfo->frags[i]);

		num_descs += gve_num_descs_per_buf(frag_size);
	}

	return num_descs;
}

/* Returns true if HW is capable of sending TSO represented by `skb`.
 *
 * Each segment must not span more than GVE_TX_MAX_DATA_DESCS buffers.
 * - The header is counted as one buffer for every single segment.
 * - A buffer which is split between two segments is counted for both.
 * - If a buffer contains both header and payload, it is counted as two buffers.
 */
static bool gve_can_send_tso(const struct sk_buff *skb)
{
	const int max_bufs_per_seg = GVE_TX_MAX_DATA_DESCS - 1;
	const struct skb_shared_info *shinfo = skb_shinfo(skb);
	const int header_len = skb_tcp_all_headers(skb);
	const int gso_size = shinfo->gso_size;
	int cur_seg_num_bufs;
	int prev_frag_size;
	int cur_seg_size;
	int i;

	cur_seg_size = skb_headlen(skb) - header_len;
	prev_frag_size = skb_headlen(skb);
	cur_seg_num_bufs = cur_seg_size > 0;

	for (i = 0; i < shinfo->nr_frags; i++) {
		if (cur_seg_size >= gso_size) {
			cur_seg_size %= gso_size;
			cur_seg_num_bufs = cur_seg_size > 0;

			if (prev_frag_size > GVE_TX_MAX_BUF_SIZE_DQO) {
				int prev_frag_remain = prev_frag_size %
					GVE_TX_MAX_BUF_SIZE_DQO;

				/* If the last descriptor of the previous frag
				 * is less than cur_seg_size, the segment will
				 * span two descriptors in the previous frag.
				 * Since max gso size (9728) is less than
				 * GVE_TX_MAX_BUF_SIZE_DQO, it is impossible
				 * for the segment to span more than two
				 * descriptors.
				 */
				if (prev_frag_remain &&
				    cur_seg_size > prev_frag_remain)
					cur_seg_num_bufs++;
			}
		}

		if (unlikely(++cur_seg_num_bufs > max_bufs_per_seg))
			return false;

		prev_frag_size = skb_frag_size(&shinfo->frags[i]);
		cur_seg_size += prev_frag_size;
	}

	return true;
}

netdev_features_t gve_features_check_dqo(struct sk_buff *skb,
					 struct net_device *dev,
					 netdev_features_t features)
{
	if (skb_is_gso(skb) && !gve_can_send_tso(skb))
		return features & ~NETIF_F_GSO_MASK;

	return features;
}

/* Attempt to transmit specified SKB.
 *
 * Returns 0 if the SKB was transmitted or dropped.
 * Returns -1 if there is not currently enough space to transmit the SKB.
 */
static int gve_try_tx_skb(struct gve_priv *priv, struct gve_tx_ring *tx,
			  struct sk_buff *skb)
{
	int num_buffer_descs;
	int total_num_descs;

	if (skb_is_gso(skb) && unlikely(ipv6_hopopt_jumbo_remove(skb)))
		goto drop;

	if (tx->dqo.qpl) {
		/* We do not need to verify the number of buffers used per
		 * packet or per segment in case of TSO as with 2K size buffers
		 * none of the TX packet rules would be violated.
		 *
		 * gve_can_send_tso() checks that each TCP segment of gso_size is
		 * not distributed over more than 9 SKB frags..
		 */
		num_buffer_descs = DIV_ROUND_UP(skb->len, GVE_TX_BUF_SIZE_DQO);
	} else {
		num_buffer_descs = gve_num_buffer_descs_needed(skb);
		if (!skb_is_gso(skb)) {
			if (unlikely(num_buffer_descs > GVE_TX_MAX_DATA_DESCS)) {
				if (unlikely(skb_linearize(skb) < 0))
					goto drop;

				num_buffer_descs = 1;
			}
		}
	}

	/* Metadata + (optional TSO) + data descriptors. */
	total_num_descs = 1 + skb_is_gso(skb) + num_buffer_descs;
	if (unlikely(gve_maybe_stop_tx_dqo(tx, total_num_descs +
			GVE_TX_MIN_DESC_PREVENT_CACHE_OVERLAP,
			num_buffer_descs))) {
		return -1;
	}

	if (unlikely(gve_tx_add_skb_dqo(tx, skb) < 0))
		goto drop;

	netdev_tx_sent_queue(tx->netdev_txq, skb->len);
	skb_tx_timestamp(skb);
	return 0;

drop:
	tx->dropped_pkt++;
	dev_kfree_skb_any(skb);
	return 0;
}

/* Transmit a given skb and ring the doorbell. */
netdev_tx_t gve_tx_dqo(struct sk_buff *skb, struct net_device *dev)
{
	struct gve_priv *priv = netdev_priv(dev);
	struct gve_tx_ring *tx;

	tx = &priv->tx[skb_get_queue_mapping(skb)];
	if (unlikely(gve_try_tx_skb(priv, tx, skb) < 0)) {
		/* We need to ring the txq doorbell -- we have stopped the Tx
		 * queue for want of resources, but prior calls to gve_tx()
		 * may have added descriptors without ringing the doorbell.
		 */
		gve_tx_put_doorbell_dqo(priv, tx->q_resources, tx->dqo_tx.tail);
		return NETDEV_TX_BUSY;
	}

	if (!netif_xmit_stopped(tx->netdev_txq) && netdev_xmit_more())
		return NETDEV_TX_OK;

	gve_tx_put_doorbell_dqo(priv, tx->q_resources, tx->dqo_tx.tail);
	return NETDEV_TX_OK;
}

static void add_to_list(struct gve_tx_ring *tx, struct gve_index_list *list,
			struct gve_tx_pending_packet_dqo *pending_packet)
{
	s16 old_tail, index;

	index = pending_packet - tx->dqo.pending_packets;
	old_tail = list->tail;
	list->tail = index;
	if (old_tail == -1)
		list->head = index;
	else
		tx->dqo.pending_packets[old_tail].next = index;

	pending_packet->next = -1;
	pending_packet->prev = old_tail;
}

static void remove_from_list(struct gve_tx_ring *tx,
			     struct gve_index_list *list,
			     struct gve_tx_pending_packet_dqo *pkt)
{
	s16 prev_index, next_index;

	prev_index = pkt->prev;
	next_index = pkt->next;

	if (prev_index == -1) {
		/* Node is head */
		list->head = next_index;
	} else {
		tx->dqo.pending_packets[prev_index].next = next_index;
	}
	if (next_index == -1) {
		/* Node is tail */
		list->tail = prev_index;
	} else {
		tx->dqo.pending_packets[next_index].prev = prev_index;
	}
}

static void gve_unmap_packet(struct device *dev,
			     struct gve_tx_pending_packet_dqo *pkt)
{
	int i;

	/* SKB linear portion is guaranteed to be mapped */
	dma_unmap_single(dev, dma_unmap_addr(pkt, dma[0]),
			 dma_unmap_len(pkt, len[0]), DMA_TO_DEVICE);
	for (i = 1; i < pkt->num_bufs; i++) {
		dma_unmap_page(dev, dma_unmap_addr(pkt, dma[i]),
			       dma_unmap_len(pkt, len[i]), DMA_TO_DEVICE);
	}
	pkt->num_bufs = 0;
}

/* Completion types and expected behavior:
 * No Miss compl + Packet compl = Packet completed normally.
 * Miss compl + Re-inject compl = Packet completed normally.
 * No Miss compl + Re-inject compl = Skipped i.e. packet not completed.
 * Miss compl + Packet compl = Skipped i.e. packet not completed.
 */
static void gve_handle_packet_completion(struct gve_priv *priv,
					 struct gve_tx_ring *tx, bool is_napi,
					 u16 compl_tag, u64 *bytes, u64 *pkts,
					 bool is_reinjection)
{
	struct gve_tx_pending_packet_dqo *pending_packet;

	if (unlikely(compl_tag >= tx->dqo.num_pending_packets)) {
		net_err_ratelimited("%s: Invalid TX completion tag: %d\n",
				    priv->dev->name, (int)compl_tag);
		return;
	}

	pending_packet = &tx->dqo.pending_packets[compl_tag];

	if (unlikely(is_reinjection)) {
		if (unlikely(pending_packet->state ==
			     GVE_PACKET_STATE_TIMED_OUT_COMPL)) {
			net_err_ratelimited("%s: Re-injection completion: %d received after timeout.\n",
					    priv->dev->name, (int)compl_tag);
			/* Packet was already completed as a result of timeout,
			 * so just remove from list and free pending packet.
			 */
			remove_from_list(tx,
					 &tx->dqo_compl.timed_out_completions,
					 pending_packet);
			gve_free_pending_packet(tx, pending_packet);
			return;
		}
		if (unlikely(pending_packet->state !=
			     GVE_PACKET_STATE_PENDING_REINJECT_COMPL)) {
			/* No outstanding miss completion but packet allocated
			 * implies packet receives a re-injection completion
			 * without a prior miss completion. Return without
			 * completing the packet.
			 */
			net_err_ratelimited("%s: Re-injection completion received without corresponding miss completion: %d\n",
					    priv->dev->name, (int)compl_tag);
			return;
		}
		remove_from_list(tx, &tx->dqo_compl.miss_completions,
				 pending_packet);
	} else {
		/* Packet is allocated but not a pending data completion. */
		if (unlikely(pending_packet->state !=
			     GVE_PACKET_STATE_PENDING_DATA_COMPL)) {
			net_err_ratelimited("%s: No pending data completion: %d\n",
					    priv->dev->name, (int)compl_tag);
			return;
		}
	}
	tx->dqo_tx.completed_packet_desc_cnt += pending_packet->num_bufs;
	if (tx->dqo.qpl)
		gve_free_tx_qpl_bufs(tx, pending_packet);
	else
		gve_unmap_packet(tx->dev, pending_packet);

	*bytes += pending_packet->skb->len;
	(*pkts)++;
	napi_consume_skb(pending_packet->skb, is_napi);
	pending_packet->skb = NULL;
	gve_free_pending_packet(tx, pending_packet);
}

static void gve_handle_miss_completion(struct gve_priv *priv,
				       struct gve_tx_ring *tx, u16 compl_tag,
				       u64 *bytes, u64 *pkts)
{
	struct gve_tx_pending_packet_dqo *pending_packet;

	if (unlikely(compl_tag >= tx->dqo.num_pending_packets)) {
		net_err_ratelimited("%s: Invalid TX completion tag: %d\n",
				    priv->dev->name, (int)compl_tag);
		return;
	}

	pending_packet = &tx->dqo.pending_packets[compl_tag];
	if (unlikely(pending_packet->state !=
				GVE_PACKET_STATE_PENDING_DATA_COMPL)) {
		net_err_ratelimited("%s: Unexpected packet state: %d for completion tag : %d\n",
				    priv->dev->name, (int)pending_packet->state,
				    (int)compl_tag);
		return;
	}

	pending_packet->state = GVE_PACKET_STATE_PENDING_REINJECT_COMPL;
	/* jiffies can wraparound but time comparisons can handle overflows. */
	pending_packet->timeout_jiffies =
			jiffies +
			msecs_to_jiffies(GVE_REINJECT_COMPL_TIMEOUT *
					 MSEC_PER_SEC);
	add_to_list(tx, &tx->dqo_compl.miss_completions, pending_packet);

	*bytes += pending_packet->skb->len;
	(*pkts)++;
}

static void remove_miss_completions(struct gve_priv *priv,
				    struct gve_tx_ring *tx)
{
	struct gve_tx_pending_packet_dqo *pending_packet;
	s16 next_index;

	next_index = tx->dqo_compl.miss_completions.head;
	while (next_index != -1) {
		pending_packet = &tx->dqo.pending_packets[next_index];
		next_index = pending_packet->next;
		/* Break early because packets should timeout in order. */
		if (time_is_after_jiffies(pending_packet->timeout_jiffies))
			break;

		remove_from_list(tx, &tx->dqo_compl.miss_completions,
				 pending_packet);
		/* Unmap/free TX buffers and free skb but do not unallocate packet i.e.
		 * the completion tag is not freed to ensure that the driver
		 * can take appropriate action if a corresponding valid
		 * completion is received later.
		 */
		if (tx->dqo.qpl)
			gve_free_tx_qpl_bufs(tx, pending_packet);
		else
			gve_unmap_packet(tx->dev, pending_packet);

		/* This indicates the packet was dropped. */
		dev_kfree_skb_any(pending_packet->skb);
		pending_packet->skb = NULL;
		tx->dropped_pkt++;
		net_err_ratelimited("%s: No reinjection completion was received for: %d.\n",
				    priv->dev->name,
				    (int)(pending_packet - tx->dqo.pending_packets));

		pending_packet->state = GVE_PACKET_STATE_TIMED_OUT_COMPL;
		pending_packet->timeout_jiffies =
				jiffies +
				msecs_to_jiffies(GVE_DEALLOCATE_COMPL_TIMEOUT *
						 MSEC_PER_SEC);
		/* Maintain pending packet in another list so the packet can be
		 * unallocated at a later time.
		 */
		add_to_list(tx, &tx->dqo_compl.timed_out_completions,
			    pending_packet);
	}
}

static void remove_timed_out_completions(struct gve_priv *priv,
					 struct gve_tx_ring *tx)
{
	struct gve_tx_pending_packet_dqo *pending_packet;
	s16 next_index;

	next_index = tx->dqo_compl.timed_out_completions.head;
	while (next_index != -1) {
		pending_packet = &tx->dqo.pending_packets[next_index];
		next_index = pending_packet->next;
		/* Break early because packets should timeout in order. */
		if (time_is_after_jiffies(pending_packet->timeout_jiffies))
			break;

		remove_from_list(tx, &tx->dqo_compl.timed_out_completions,
				 pending_packet);
		gve_free_pending_packet(tx, pending_packet);
	}
}

int gve_clean_tx_done_dqo(struct gve_priv *priv, struct gve_tx_ring *tx,
			  struct napi_struct *napi)
{
	u64 reinject_compl_bytes = 0;
	u64 reinject_compl_pkts = 0;
	int num_descs_cleaned = 0;
	u64 miss_compl_bytes = 0;
	u64 miss_compl_pkts = 0;
	u64 pkt_compl_bytes = 0;
	u64 pkt_compl_pkts = 0;

	/* Limit in order to avoid blocking for too long */
	while (!napi || pkt_compl_pkts < napi->weight) {
		struct gve_tx_compl_desc *compl_desc =
			&tx->dqo.compl_ring[tx->dqo_compl.head];
		u16 type;

		if (compl_desc->generation == tx->dqo_compl.cur_gen_bit)
			break;

		/* Prefetch the next descriptor. */
		prefetch(&tx->dqo.compl_ring[(tx->dqo_compl.head + 1) &
				tx->dqo.complq_mask]);

		/* Do not read data until we own the descriptor */
		dma_rmb();
		type = compl_desc->type;

		if (type == GVE_COMPL_TYPE_DQO_DESC) {
			/* This is the last descriptor fetched by HW plus one */
			u16 tx_head = le16_to_cpu(compl_desc->tx_head);

			atomic_set_release(&tx->dqo_compl.hw_tx_head, tx_head);
		} else if (type == GVE_COMPL_TYPE_DQO_PKT) {
			u16 compl_tag = le16_to_cpu(compl_desc->completion_tag);
			if (compl_tag & GVE_ALT_MISS_COMPL_BIT) {
				compl_tag &= ~GVE_ALT_MISS_COMPL_BIT;
				gve_handle_miss_completion(priv, tx, compl_tag,
							   &miss_compl_bytes,
							   &miss_compl_pkts);
			} else {
				gve_handle_packet_completion(priv, tx, !!napi,
							     compl_tag,
							     &pkt_compl_bytes,
							     &pkt_compl_pkts,
							     false);
			}
		} else if (type == GVE_COMPL_TYPE_DQO_MISS) {
			u16 compl_tag = le16_to_cpu(compl_desc->completion_tag);

			gve_handle_miss_completion(priv, tx, compl_tag,
						   &miss_compl_bytes,
						   &miss_compl_pkts);
		} else if (type == GVE_COMPL_TYPE_DQO_REINJECTION) {
			u16 compl_tag = le16_to_cpu(compl_desc->completion_tag);

			gve_handle_packet_completion(priv, tx, !!napi,
						     compl_tag,
						     &reinject_compl_bytes,
						     &reinject_compl_pkts,
						     true);
		}

		tx->dqo_compl.head =
			(tx->dqo_compl.head + 1) & tx->dqo.complq_mask;
		/* Flip the generation bit when we wrap around */
		tx->dqo_compl.cur_gen_bit ^= tx->dqo_compl.head == 0;
		num_descs_cleaned++;
	}

	netdev_tx_completed_queue(tx->netdev_txq,
				  pkt_compl_pkts + miss_compl_pkts,
				  pkt_compl_bytes + miss_compl_bytes);

	remove_miss_completions(priv, tx);
	remove_timed_out_completions(priv, tx);

	u64_stats_update_begin(&tx->statss);
	tx->bytes_done += pkt_compl_bytes + reinject_compl_bytes;
	tx->pkt_done += pkt_compl_pkts + reinject_compl_pkts;
	u64_stats_update_end(&tx->statss);
	return num_descs_cleaned;
}

bool gve_tx_poll_dqo(struct gve_notify_block *block, bool do_clean)
{
	struct gve_tx_compl_desc *compl_desc;
	struct gve_tx_ring *tx = block->tx;
	struct gve_priv *priv = block->priv;

	if (do_clean) {
		int num_descs_cleaned = gve_clean_tx_done_dqo(priv, tx,
							      &block->napi);

		/* Sync with queue being stopped in `gve_maybe_stop_tx_dqo()` */
		mb();

		if (netif_tx_queue_stopped(tx->netdev_txq) &&
		    num_descs_cleaned > 0) {
			tx->wake_queue++;
			netif_tx_wake_queue(tx->netdev_txq);
		}
	}

	/* Return true if we still have work. */
	compl_desc = &tx->dqo.compl_ring[tx->dqo_compl.head];
	return compl_desc->generation != tx->dqo_compl.cur_gen_bit;
}
