/*
* Copyright (c) 2016-2018 Mindaugas Rasiukevicius <rmind at noxt eu>
* All rights reserved.
*
* Use is subject to license terms, as specified in the LICENSE file.
*/

/*
* Epoch-based reclamation (EBR).  Reference:
*
*	K. Fraser, Practical lock-freedom,
*	Technical Report UCAM-CL-TR-579, February 2004
*	https://www.cl.cam.ac.uk/techreports/UCAM-CL-TR-579.pdf
*
* Summary:
*
* Any workers (threads or processes) actively referencing (accessing)
* the globally visible objects must do that in the critical path covered
* using the dedicated enter/exit functions.  The grace period is
* determined using "epochs" -- implemented as a global counter (and,
* for example, a dedicated G/C list for each epoch).  Objects in the
* current global epoch can be staged for reclamation (garbage collection).
* Then, the objects in the target epoch can be reclaimed after two
* successful increments of the global epoch.  Only three epochs are
* needed (e, e-1 and e-2), therefore we use clock arithmetics.
*
* See the comments in the ebr_sync() function for detailed explanation.
*/

#include <assert.h>
#include <atomic>
#include "PMwCAS.h"
#include "ebr.h"

#define	ACTIVE_FLAG		(0x80000000U)

typedef struct ebr_tls {
	/*
	* - A local epoch counter for each thread.
	* - The epoch counter may have the "active" flag set.
	* - Thread list entry (pointer).
	*/
	unsigned		local_epoch;
	struct ebr_tls *	next;
} ebr_tls_t;

thread_local ebr_tls_t * local_ebr = nullptr;

struct ebr {
	/*
	* - There is a global epoch counter which can be 0, 1 or 2.
	* - TLS with a list of the registered threads.
	*/
	unsigned		global_epoch;
	ebr_tls_t *		list;
};

ebr_t *
ebr_create(void)
{
	ebr_t *ebr;

	if ((ebr = (ebr_t *)calloc(1, sizeof(ebr_t))) == NULL) {
		return NULL;
	}
	return ebr;
}

void
ebr_destroy(ebr_t *ebr)
{
	free(ebr);
}

/*
* ebr_register: register the current worker (thread/process) for EBR.
*
* => Returns 0 on success and errno on failure.
*/
int
ebr_register(ebr_t *ebr)
{
	ebr_tls_t *head;
	if (local_ebr)
		return 0;
	if ((local_ebr = (ebr_tls_t *)malloc(sizeof(ebr_tls_t))) == NULL) {
		return -1;
	}
	//cout << "alloc addr " << hex << local_ebr << endl;
	memset(local_ebr, 0, sizeof(ebr_tls_t));
	uint64_t r;
	do {
		head = ebr->list;
		//cout << "ini read " << hex << ebr->list << endl;
		local_ebr->next = head;
		r = CAS((uint64_t*)&ebr->list, (uint64_t)local_ebr, (uint64_t)head);
		//cout << "r CAS " << hex << r << endl;

	} while (r != (uint64_t)head);
//	cout << "final " << hex << ebr->list << endl;
	return 0;
}

/*
* ebr_enter: mark the entrance to the critical path.
*/
void
ebr_enter(ebr_t *ebr)
{
	assert(local_ebr);

	/*
	* Set the "active" flag and set the local epoch to global
	* epoch (i.e. observe the global epoch).  Ensure that the
	* epoch is observed before any loads in the critical path.
	*/
	local_ebr->local_epoch = ebr->global_epoch | ACTIVE_FLAG;
	std::atomic_thread_fence(std::memory_order_seq_cst);
}

/*
* ebr_exit: mark the exit of the critical path.
*/
void
ebr_exit(ebr_t *ebr)
{
	ebr_tls_t *t;

	t = local_ebr;
	assert(t != NULL);

	/*
	* Clear the "active" flag.  Must ensure that any stores in
	* the critical path reach global visibility before that.
	*/
	std::atomic_thread_fence(std::memory_order_seq_cst);
	assert(t->local_epoch & ACTIVE_FLAG);
	t->local_epoch = 0;
}

/*
* ebr_sync: attempt to synchronise and announce a new epoch.
*
* => Synchronisation points must be serialised.
* => Return true if a new epoch was announced.
* => Return the epoch ready for reclamation.
*/
bool
ebr_sync(ebr_t *ebr, unsigned *gc_epoch)
{
	unsigned epoch;
	ebr_tls_t *t;

	/*
	* Ensure that any loads or stores on the writer side reach
	* the global visibility.  We want to allow the callers to
	* assume that the ebr_sync() call serves as a full barrier.
	*/
	epoch = ebr->global_epoch;
	std::atomic_thread_fence(std::memory_order_seq_cst);

	/*
	* Check whether all active workers observed the global epoch.
	*/
	t = ebr->list;
	while (t) {
		const unsigned local_epoch = t->local_epoch; // atomic fetch
		const bool active = (local_epoch & ACTIVE_FLAG) != 0;

		if (active && (local_epoch != (epoch | ACTIVE_FLAG))) {
			/* No, not ready. */
			*gc_epoch = ebr_gc_epoch(ebr);
			return false;
		}
		t = t->next;
	}

	/* Yes: increment and announce a new global epoch. */
	ebr->global_epoch = (epoch + 1) % 3;

	/*
	* Let the new global epoch be 'e'.  At this point:
	*
	* => Active workers: might still be running in the critical path
	*    in the e-1 epoch or might be already entering a new critical
	*    path and observing the new epoch e.
	*
	* => Inactive workers: might become active by entering a critical
	*    path before or after the global epoch counter was incremented,
	*    observing either e-1 or e.
	*
	* => Note that the active workers cannot have a stale observation
	*    of the e-2 epoch at this point (there is no ABA problem using
	*    the clock arithmetics).
	*
	* => Therefore, there can be no workers still running the critical
	*    path in the e-2 epoch.  This is the epoch ready for G/C.
	*/
	*gc_epoch = ebr_gc_epoch(ebr);
	return true;
}

/*
* ebr_staging_epoch: return the epoch where objects can be staged
* for reclamation.
*/
unsigned
ebr_staging_epoch(ebr_t *ebr)
{
	/* The current epoch. */
	return ebr->global_epoch;
}

/*
* ebr_gc_epoch: return the epoch where objects are ready to be
* reclaimed i.e. it is guaranteed to be safe to destroy them.
*/
unsigned
ebr_gc_epoch(ebr_t *ebr)
{
	/*
	* Since we use only 3 epochs, e-2 is just the next global
	* epoch with clock arithmetics.
	*/
	return (ebr->global_epoch + 1) % 3;
}
