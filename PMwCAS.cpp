#include "spin_latch.h"
#include "PMwCAS.h"

void init_pool(mdesc_pool_t pool)
{
	for (off_t i = 0; i < DESCRIPTOR_POOL_SIZE; ++i)
	{
		pool->mdescs[i].status = ST_FREE;
		persist(&pool->mdescs[i].status, sizeof(pool->mdescs[i].status));
	}
}

mdesc_t alloc_PMwCAS(mdesc_pool_t pool, off_t search_pos) 
{
	for (off_t i = 0; i < DESCRIPTOR_POOL_SIZE; ++i)
	{
		off_t pos = (i + search_pos) % DESCRIPTOR_POOL_SIZE;
		if (pool->mdescs[pos].status == ST_FREE)
		{
			/*
			* set dirty bit in status
			* in case a failure occurs before safe memory transfer
			* recovery may reclaim descriptors with dirty bit set
			* alloc user needs to persist and unset dirty bit
			* before any use after the alloc called
			*/
			uint64_t r = CAS(&pool->mdescs[pos].status, ST_UNDECIDED | DIRTY_BIT, ST_FREE);
			if (r == ST_FREE)
			{
				pool->mdescs[pos].count = 0;
				persist((uint64_t*)&pool->mdescs[pos].count, sizeof(uint64_t));
				return &pool->mdescs[pos];
			}
		}
	}
	return nullptr;
}

void free_PMwCAS(mdesc_t mdesc, gc_t * gc) 
{
	gc_limbo(gc, (void*)mdesc);
}

void reclaim_PMwCAS(gc_entry_t *entry, void *arg)
{
	gc_t *gc = (gc_t *)arg;
	const off_t off = gc->entry_off;
	uint64_t *status_addr;

	while (entry) {
		status_addr = (uint64_t *)((uintptr_t)entry - off);
		entry = entry->next;
		
		/*
		* no race condition here
		*/
		*status_addr = ST_FREE;
		persist(status_addr, sizeof(*status_addr));
	}
}

/*
* single thread function
* we don't persist memory until the app caller
* issues a PMwCAS
*/
bool add_entry(PMEMobjpool *pop, mdesc_t mdesc, uint64_t * addr, uint64_t expect, uint64_t new_val, off_t recycle) 
{
	off_t insert_point = (off_t)mdesc->count, i;
	wdesc_t wdesc = mdesc->wdescs;
	/* check if PMwCAS is full */
	if (mdesc->count == WORD_DESCRIPTOR_SIZE)
	{
		return false;
	}
	/* 
	* check if the target address exists
	* otherwise, find the insert point 
	*/
	for (i = 0; i < mdesc->count; ++i)
	{
		wdesc = mdesc->wdescs + i;
		if (wdesc->addr == addr)
		{
			return false;
		}
		if (wdesc->addr > addr && insert_point > i)
		{
			insert_point = i;
		}
	}
	TX_BEGIN(pop) {
		pmemobj_tx_add_range_direct(mdesc->wdescs + insert_point, 
			(mdesc->count - insert_point + 1) * sizeof(*mdesc->wdescs));
		if (insert_point != mdesc->count)
			memmove(mdesc->wdescs + insert_point + 1,
				mdesc->wdescs + insert_point, 
				(mdesc->count - insert_point) * sizeof(*mdesc->wdescs));
		
		mdesc->wdescs[insert_point].addr = addr;
		mdesc->wdescs[insert_point].expect = expect;
		mdesc->wdescs[insert_point].new_val = new_val;
		mdesc->wdescs[insert_point].mdesc = mdesc;
		mdesc->wdescs[insert_point].recycle_func = recycle;
		
		pmemobj_tx_add_range_direct(&mdesc->count, sizeof(uint64_t));
		++mdesc->count;
	} TX_END;
	
	return true;
}

void *reserve_entry(PMEMobjpool *pop, mdesc_t mdesc, uint64_t *addr, uint64_t expect, off_t recycle)
{
	off_t i;
	wdesc_t wdesc;
	if (mdesc->count == WORD_DESCRIPTOR_SIZE)
	{
		return false;
	}
	for (i = 0; i < mdesc->count; ++i)
	{
		wdesc = mdesc->wdescs + i;
		if (wdesc->addr > addr)
		{
			break;
		}
	}
	TX_BEGIN(pop) {
		pmemobj_tx_add_range_direct(mdesc->wdescs + i, 
			(mdesc->count - i + 1) * sizeof(mdesc->wdescs[i]));
		
		memmove(wdesc + 1, wdesc, (mdesc->count - i) * sizeof(*wdesc));
		
		mdesc->wdescs[i].addr = addr;
		mdesc->wdescs[i].expect = expect;
		mdesc->wdescs[i].mdesc = mdesc;
		mdesc->wdescs[i].recycle_func = recycle;
		
		pmemobj_tx_add_range_direct(&mdesc->count, sizeof(uint64_t));
		++mdesc->count;
	} TX_END;
	return (void *)&mdesc->wdescs[i].new_val;
}

bool is_RDCSS(uint64_t val)
{
	return (val & RDCSS_BIT) != 0ULL;
}

bool is_MwCAS(uint64_t val)
{
	return (val & MwCAS_BIT) != 0ULL;
}

bool is_dirty(uint64_t val)
{
	return (val & DIRTY_BIT) != 0ULL;
}

void persist_clear_dirty_bit(uint64_t *addr, uint64_t val)
{
	persist(addr, sizeof(uint64_t));
	CAS(addr, val & ~DIRTY_BIT, val);
}

void complete_install(wdesc_t wdesc)
{
	uint64_t mdesc_ptr = (uint64_t)wdesc->mdesc | MwCAS_BIT | DIRTY_BIT;
	uint64_t wdesc_ptr = (uint64_t)wdesc | RDCSS_BIT;
	bool test = wdesc->mdesc->status == ST_UNDECIDED;
	CAS(wdesc->addr, test ? mdesc_ptr : wdesc->expect, wdesc_ptr);
}

uint64_t install_mdesc(wdesc_t wdesc)
{
	uint64_t ptr = (uint64_t)wdesc | RDCSS_BIT;
	uint64_t r;
	do 
	{
		r = CAS(wdesc->addr, ptr, wdesc->expect);
		if (is_RDCSS(r))
		{
			complete_install(wdesc_t(r & ADDR_MASK));
			continue;
		}
		if (r == wdesc->expect)
		{
			complete_install(wdesc);
		}
	} while (false);
	return r;
}

bool PMwCAS(mdesc_t mdesc)
{
	/* clear dirty bit in status and start PMwCAS */
	persist_clear_dirty_bit((uint64_t*)&mdesc->status, ST_UNDECIDED | DIRTY_BIT);

	uint64_t status = ST_SUCCESS;
	for (off_t i = 0; status == ST_SUCCESS && i < mdesc->count; ++i) 
	{
		wdesc_t wdesc = mdesc->wdescs + i;
		do {
			/*
			* try to install a pointer to mdesc for tha target word
			*/
			uint64_t r = install_mdesc(wdesc);
			if (r == wdesc->expect || (r & ADDR_MASK) == (uint64_t)mdesc)
			{
				/*
				* successful CAS install
				* or has been installed by another thread
				*/
				break;
			}
			if (is_MwCAS(r))
			{
				/*
				* read a multi word decriptor
				* help it finish
				*/
				if (is_dirty(r))
				{
					/*
					* make sure what we read is persistent
					*/
					persist_clear_dirty_bit(wdesc->addr, r);
				}
				PMwCAS(mdesc_t(r & ADDR_MASK));
				/*
				* retry install
				*/
				continue;
			}
			/*
			* otherwise, CAS failed, so the whole PMwCAS fails
			*/
			status = ST_FAILED;
		} while (false);
	}
	uint64_t mdesc_ptr = (uint64_t)mdesc | DIRTY_BIT | MwCAS_BIT;
	
	/*
	* make sure that every target word is installed
	*/
	if (status == ST_SUCCESS)
	{
		for (off_t i = 0; i < mdesc->count; ++i)
		{
			wdesc_t wdesc = mdesc->wdescs + i;
			persist_clear_dirty_bit(wdesc->addr, mdesc_ptr);
		}
	}

	/*
	* finalize MwCAS status
	*/
	CAS(&mdesc->status, status | DIRTY_BIT, ST_UNDECIDED);
	persist_clear_dirty_bit(&mdesc->status, mdesc->status);

	/*
	* install the final value for each word
	*/
	for (off_t i = 0; i < mdesc->count; ++i)
	{
		wdesc_t wdesc = mdesc->wdescs + i;
		uint64_t val = 
			(mdesc->status == ST_SUCCESS ? wdesc->new_val : wdesc->expect) | DIRTY_BIT;
		uint64_t r = CAS(wdesc->addr, val, mdesc_ptr);
		
		/*
		* if the dirty bit has been unset
		*/
		if (r == (mdesc_ptr & ~DIRTY_BIT))
		{
			CAS(wdesc->addr, val, mdesc_ptr & ~DIRTY_BIT);
		}
		persist_clear_dirty_bit(wdesc->addr, val);
	}
	return mdesc->status == ST_SUCCESS;
}

uint64_t pread(uint64_t * addr)
{
	uint64_t r;
	do 
	{
		r = *addr;
		if (is_RDCSS(r))
		{
			complete_install(wdesc_t(r & ADDR_MASK));
			continue;
		}
		if (is_dirty(r))
		{
			persist_clear_dirty_bit(addr, r);
			r &= ~DIRTY_BIT;
		}
		if (is_MwCAS(r))
		{
			PMwCAS(mdesc_t(r & ADDR_MASK));
			continue;
		}
	} while (false);
	return r;
}

/*
* recovery process(in a single thread manner):
* 1) roll back failed or in-flight PMwCAS
* 2) finish success PMwCAS
* 3) reclaim PMwCAS descriptor
*/
void recovery_PMwCAS(mdesc_pool_t pool)
{
	for (off_t i = 0; i < DESCRIPTOR_POOL_SIZE; ++i)
	{
		mdesc_t mdesc = pool->mdescs + i;
		/*
		* clear dirty bit in persistent memory,
		* this is because crash happens before CPU flushes the newest cache line
		* back to persistent memory
		*/
		if (is_dirty(mdesc->status))
		{
			mdesc->status &= ~DIRTY_BIT;
			persist(&mdesc->status, sizeof(mdesc->status));
		}
		if (mdesc->status == ST_FREE)
		{
			continue;
		}
		bool done = mdesc->status == ST_SUCCESS;
		uint64_t mdesc_ptr = (uint64_t)mdesc | MwCAS_BIT | DIRTY_BIT;

		/*
		* each target word could remain:
		* 1) old value
		* 2) ptr to word descriptor
		* 3) ptr to multi-word descriptor
		* 4) new val
		*/
		for (off_t j = 0; j < mdesc->count; ++j)
		{
			wdesc_t wdesc = mdesc->wdescs + j;
			uint64_t r, val = done ? wdesc->new_val : wdesc->expect;

			/* case (3) when dirty bit set */
			r = CAS(wdesc->addr, wdesc->expect, mdesc_ptr);
			/* case (3) when the dirty bit unset */
			if (r == (mdesc_ptr & ~DIRTY_BIT))
			{
				CAS(wdesc->addr, wdesc->new_val, mdesc_ptr & ~DIRTY_BIT);
			}
			/* case (2) */
			if (r == ((uint64_t)wdesc | RDCSS_BIT))
			{
				CAS(wdesc->addr, wdesc->expect, (uint64_t)wdesc | RDCSS_BIT);
			}
			/*
			* if all CASs above fail,
			* target word remain in case (1) or case (4)
			* no need to modify
			*/
			persist(wdesc->addr, sizeof(*wdesc->addr));
		}
		/* we have persist all the target words to the correct state */
		mdesc->status = ST_FREE;
		persist(&mdesc->status, sizeof(mdesc->status));
	}
}
