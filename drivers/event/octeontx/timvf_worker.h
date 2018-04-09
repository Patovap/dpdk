/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2017 Cavium, Inc
 */

#include <rte_common.h>
#include <rte_branch_prediction.h>

#include "timvf_evdev.h"

static inline int16_t
timr_bkt_fetch_rem(uint64_t w1)
{
	return (w1 >> TIM_BUCKET_W1_S_CHUNK_REMAINDER) &
		TIM_BUCKET_W1_M_CHUNK_REMAINDER;
}

static inline int16_t
timr_bkt_get_rem(struct tim_mem_bucket *bktp)
{
	return __atomic_load_n(&bktp->chunk_remainder,
			__ATOMIC_ACQUIRE);
}

static inline void
timr_bkt_set_rem(struct tim_mem_bucket *bktp, uint16_t v)
{
	__atomic_store_n(&bktp->chunk_remainder, v,
			__ATOMIC_RELEASE);
}

static inline void
timr_bkt_sub_rem(struct tim_mem_bucket *bktp, uint16_t v)
{
	__atomic_fetch_sub(&bktp->chunk_remainder, v,
			__ATOMIC_RELEASE);
}

static inline uint8_t
timr_bkt_get_sbt(uint64_t w1)
{
	return (w1 >> TIM_BUCKET_W1_S_SBT) & TIM_BUCKET_W1_M_SBT;
}

static inline uint64_t
timr_bkt_set_sbt(struct tim_mem_bucket *bktp)
{
	const uint64_t v = TIM_BUCKET_W1_M_SBT << TIM_BUCKET_W1_S_SBT;
	return __atomic_fetch_or(&bktp->w1, v, __ATOMIC_ACQ_REL);
}

static inline uint64_t
timr_bkt_clr_sbt(struct tim_mem_bucket *bktp)
{
	const uint64_t v = ~(TIM_BUCKET_W1_M_SBT << TIM_BUCKET_W1_S_SBT);
	return __atomic_fetch_and(&bktp->w1, v, __ATOMIC_ACQ_REL);
}

static inline uint8_t
timr_bkt_get_shbt(uint64_t w1)
{
	return ((w1 >> TIM_BUCKET_W1_S_HBT) & TIM_BUCKET_W1_M_HBT) |
		((w1 >> TIM_BUCKET_W1_S_SBT) & TIM_BUCKET_W1_M_SBT);
}

static inline uint8_t
timr_bkt_get_hbt(uint64_t w1)
{
	return (w1 >> TIM_BUCKET_W1_S_HBT) & TIM_BUCKET_W1_M_HBT;
}

static inline uint8_t
timr_bkt_get_bsk(uint64_t w1)
{
	return (w1 >> TIM_BUCKET_W1_S_BSK) & TIM_BUCKET_W1_M_BSK;
}

static inline uint64_t
timr_bkt_clr_bsk(struct tim_mem_bucket *bktp)
{
	/*Clear everything except lock. */
	const uint64_t v = TIM_BUCKET_W1_M_LOCK << TIM_BUCKET_W1_S_LOCK;
	return __atomic_fetch_and(&bktp->w1, v, __ATOMIC_ACQ_REL);
}

static inline uint64_t
timr_bkt_fetch_sema_lock(struct tim_mem_bucket *bktp)
{
	return __atomic_fetch_add(&bktp->w1, TIM_BUCKET_SEMA_WLOCK,
			__ATOMIC_ACQ_REL);
}

static inline uint64_t
timr_bkt_fetch_sema(struct tim_mem_bucket *bktp)
{
	return __atomic_fetch_add(&bktp->w1, TIM_BUCKET_SEMA,
			__ATOMIC_RELAXED);
}

static inline uint64_t
timr_bkt_inc_lock(struct tim_mem_bucket *bktp)
{
	const uint64_t v = 1ull << TIM_BUCKET_W1_S_LOCK;
	return __atomic_fetch_add(&bktp->w1, v, __ATOMIC_ACQ_REL);
}

static inline void
timr_bkt_dec_lock(struct tim_mem_bucket *bktp)
{
	__atomic_add_fetch(&bktp->lock, 0xff, __ATOMIC_ACQ_REL);
}

static inline uint32_t
timr_bkt_get_nent(uint64_t w1)
{
	return (w1 >> TIM_BUCKET_W1_S_NUM_ENTRIES) &
		TIM_BUCKET_W1_M_NUM_ENTRIES;
}

static inline void
timr_bkt_inc_nent(struct tim_mem_bucket *bktp)
{
	__atomic_add_fetch(&bktp->nb_entry, 1, __ATOMIC_RELAXED);
}

static inline void
timr_bkt_add_nent(struct tim_mem_bucket *bktp, uint32_t v)
{
	__atomic_add_fetch(&bktp->nb_entry, v, __ATOMIC_RELAXED);
}

static inline uint64_t
timr_bkt_clr_nent(struct tim_mem_bucket *bktp)
{
	const uint64_t v = ~(TIM_BUCKET_W1_M_NUM_ENTRIES <<
			TIM_BUCKET_W1_S_NUM_ENTRIES);
	return __atomic_and_fetch(&bktp->w1, v, __ATOMIC_ACQ_REL);
}

static inline struct tim_mem_entry *
timr_clr_bkt(struct timvf_ring * const timr, struct tim_mem_bucket * const bkt)
{
	struct tim_mem_entry *chunk;
	struct tim_mem_entry *pnext;
	chunk = ((struct tim_mem_entry *)(uintptr_t)bkt->first_chunk);
	chunk = (struct tim_mem_entry *)(uintptr_t)(chunk + nb_chunk_slots)->w0;

	while (chunk) {
		pnext = (struct tim_mem_entry *)(uintptr_t)
			((chunk + nb_chunk_slots)->w0);
		rte_mempool_put(timr->chunk_pool, chunk);
		chunk = pnext;
	}
	return (struct tim_mem_entry *)(uintptr_t)bkt->first_chunk;
}

static inline int
timvf_rem_entry(struct rte_event_timer *tim)
{
	uint64_t lock_sema;
	struct tim_mem_entry *entry;
	struct tim_mem_bucket *bkt;
	if (tim->impl_opaque[1] == 0 ||
			tim->impl_opaque[0] == 0)
		return -ENOENT;

	entry = (struct tim_mem_entry *)(uintptr_t)tim->impl_opaque[0];
	if (entry->wqe != tim->ev.u64) {
		tim->impl_opaque[1] = tim->impl_opaque[0] = 0;
		return -ENOENT;
	}
	bkt = (struct tim_mem_bucket *)(uintptr_t)tim->impl_opaque[1];
	lock_sema = timr_bkt_inc_lock(bkt);
	if (timr_bkt_get_shbt(lock_sema)
			|| !timr_bkt_get_nent(lock_sema)) {
		timr_bkt_dec_lock(bkt);
		tim->impl_opaque[1] = tim->impl_opaque[0] = 0;
		return -ENOENT;
	}

	entry->w0 = entry->wqe = 0;
	timr_bkt_dec_lock(bkt);

	tim->state = RTE_EVENT_TIMER_CANCELED;
	tim->impl_opaque[1] = tim->impl_opaque[0] = 0;
	return 0;
}

static inline struct tim_mem_entry *
timvf_refill_chunk_generic(struct tim_mem_bucket * const bkt,
		struct timvf_ring * const timr)
{
	struct tim_mem_entry *chunk;

	if (bkt->nb_entry || !bkt->first_chunk) {
		if (unlikely(rte_mempool_get(timr->chunk_pool,
						(void **)&chunk))) {
			return NULL;
		}
		if (bkt->nb_entry) {
			*(uint64_t *)(((struct tim_mem_entry *)(uintptr_t)
					bkt->current_chunk) +
					nb_chunk_slots) =
				(uintptr_t) chunk;
		} else {
			bkt->first_chunk = (uintptr_t) chunk;
		}
	} else {
		chunk = timr_clr_bkt(timr, bkt);
		bkt->first_chunk = (uintptr_t)chunk;
	}
	*(uint64_t *)(chunk + nb_chunk_slots) = 0;

	return chunk;
}

static inline struct tim_mem_bucket *
timvf_get_target_bucket(struct timvf_ring * const timr, const uint32_t rel_bkt)
{
	const uint64_t bkt_cyc = rte_rdtsc() - timr->ring_start_cyc;
	const uint32_t bucket = rte_reciprocal_divide_u64(bkt_cyc,
			&timr->fast_div) + rel_bkt;
	const uint32_t tbkt_id = timr->get_target_bkt(bucket,
			timr->nb_bkts);
	return &timr->bkt[tbkt_id];
}

/* Multi producer functions. */
static inline int
timvf_add_entry_mp(struct timvf_ring * const timr, const uint32_t rel_bkt,
		struct rte_event_timer * const tim,
		const struct tim_mem_entry * const pent)
{
	int16_t rem;
	uint64_t lock_sema;
	struct tim_mem_bucket *bkt;
	struct tim_mem_entry *chunk;

__retry:
	bkt = timvf_get_target_bucket(timr, rel_bkt);
	/* Bucket related checks. */
	/*Get Bucket sema*/
	lock_sema = timr_bkt_fetch_sema_lock(bkt);
	if (unlikely(timr_bkt_get_shbt(lock_sema))) {
		timr_bkt_dec_lock(bkt);
		goto __retry;
	}

	rem = timr_bkt_fetch_rem(lock_sema);

	if (rem < 0) {
		/* goto diff bucket. */
		timr_bkt_dec_lock(bkt);
		goto __retry;
	} else if (!rem) {
		/*Only one thread can be here*/
		chunk = timr->refill_chunk(bkt, timr);
		if (unlikely(chunk == NULL)) {
			timr_bkt_set_rem(bkt, 0);
			timr_bkt_dec_lock(bkt);
			tim->impl_opaque[0] = tim->impl_opaque[1] = 0;
			tim->state = RTE_EVENT_TIMER_ERROR;
			return -ENOMEM;
		}
		bkt->current_chunk = (uintptr_t) chunk;
		timr_bkt_set_rem(bkt, nb_chunk_slots - 1);
	} else {
		chunk = (struct tim_mem_entry *)(uintptr_t)bkt->current_chunk;
		chunk += nb_chunk_slots - rem;
	}
	/* Copy work entry. */
	*chunk = *pent;
	timr_bkt_inc_nent(bkt);
	timr_bkt_dec_lock(bkt);

	tim->impl_opaque[0] = (uintptr_t)chunk;
	tim->impl_opaque[1] = (uintptr_t)bkt;
	tim->state = RTE_EVENT_TIMER_ARMED;
	return 0;
}
