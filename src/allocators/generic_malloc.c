/* 
 * TODO:
 * lock-free bin walking using Harris's algorithm
 * produce allocator-specific versions (dlmalloc, initially) that 
 * - don't need headers/trailers...
 * - ... by stealing bits from the host allocator's "size" field (64-bit only)
 * keep chunk lists sorted within each bin?
 */

/* This file uses GNU C extensions */
#define _GNU_SOURCE

#include <sys/types.h>
/* liballocs definitely defines these internally */
size_t malloc_usable_size(void *ptr) __attribute__((visibility("protected")));
size_t __real_malloc_usable_size(void *ptr) __attribute__((visibility("protected")));
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <strings.h>
#include <stdint.h>
#include <errno.h>
#include <assert.h>
#include <pthread.h>
#ifdef MALLOC_USABLE_SIZE_HACK
#include <dlfcn.h>
extern "C" {
static inline size_t malloc_usable_size(void *ptr) __attribute__((visibility("protected")));
}
#else
size_t malloc_usable_size(void *ptr);
#endif
#include "liballocs_private.h"

// HACK for libcrunch -- please remove (similar to malloc_usable_size -> __mallochooks_*)
void __libcrunch_uncache_all(const void *allocptr, size_t size) __attribute__((weak));

static void *allocptr_to_userptr(void *allocptr);
static void *userptr_to_allocptr(void *allocptr);

#define ALLOCPTR_TO_USERPTR(p) (allocptr_to_userptr(p))
#define USERPTR_TO_ALLOCPTR(p) (userptr_to_allocptr(p))

#define ALLOC_EVENT_QUALIFIERS __attribute__((visibility("hidden")))

#include "alloc_events.h"
#include "heap_index.h"
#include "pageindex.h"

#ifndef NO_PTHREADS
#define BIG_LOCK \
	lock_ret = pthread_mutex_lock(&mutex); \
	assert(lock_ret == 0);
#define BIG_UNLOCK \
	lock_ret = pthread_mutex_unlock(&mutex); \
	assert(lock_ret == 0);
/* We're recursive only because assertion failures sometimes want to do 
 * asprintf, so try to re-acquire our mutex. */
static pthread_mutex_t mutex = PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP;

#else
#define BIG_LOCK
#define BIG_UNLOCK
#endif

#ifndef NO_TLS
__thread void *__current_allocsite;
__thread void *__current_allocfn;
__thread size_t __current_allocsz;
__thread int __currently_freeing;
__thread int __currently_allocating;
#else
void *__current_allocsite;
void *__current_allocfn;
size_t __current_allocsz;
int __currently_freeing;
int __currently_allocating;
#endif

#ifdef MALLOC_USABLE_SIZE_HACK
#include "malloc_usable_size_hack.h"
#endif

#ifdef TRACE_HEAP_INDEX
/* Size the circular buffer of recently freed chunks */
#define RECENTLY_FREED_SIZE 100
#endif

#ifdef TRACE_HEAP_INDEX
/* Keep a circular buffer of recently freed chunks */
static void *recently_freed[RECENTLY_FREED_SIZE];
static void **next_recently_freed_to_replace = &recently_freed[0];
#endif

struct entry *index_region __attribute__((aligned(64))) /* HACK for cacheline-alignedness */;
unsigned long biggest_l1_object __attribute__((visibility("protected")));
void *index_max_address;
int safe_to_call_malloc;

void *index_begin_addr;
void *index_end_addr;
#ifndef LOOKUP_CACHE_SIZE
#define LOOKUP_CACHE_SIZE 4
#endif

struct lookup_cache_entry;
static void install_cache_entry(void *object_start,
	size_t usable_size, unsigned short depth, _Bool is_deepest,
	struct suballocated_chunk_rec *containing_chunk,
	struct insert *insert);
static void invalidate_cache_entries(void *object_start,
	unsigned short depths_mask,
	struct suballocated_chunk_rec *sub, struct insert *ins, signed nentries);
static int cache_clear_deepest_flag_and_update_ins(void *object_start,
	unsigned short depths_mask,
	struct suballocated_chunk_rec *sub, struct insert *ins, signed nentries,
	struct insert *new_ins);

static struct suballocated_chunk_rec *suballocated_chunks;

static void delete_suballocated_chunk(struct suballocated_chunk_rec *p_rec)
{
#if 0
	/* Remove it from the bitmap. */
	unsigned long *p_bitmap_word = suballocated_chunks_bitmap
			 + (p_rec - &suballocated_chunks[0]) / UNSIGNED_LONG_NBITS;
	int bit_index = (p_rec - &suballocated_chunks[0]) % UNSIGNED_LONG_NBITS;
	*p_bitmap_word &= ~(1ul<<bit_index);

	/* munmap it. */
	int ret = munmap(p_rec->metadata_recs, (sizeof (struct insert)) * p_rec->size);
	assert(ret == 0);
	ret = munmap(p_rec->starts_bitmap,
		sizeof (unsigned long) * (p_rec->real_size / UNSIGNED_LONG_NBITS));
	assert(ret == 0);
	
	// bzero the chunk rec
	bzero(p_rec, sizeof (struct suballocated_chunk_rec));
			
	/* We might want to restore the previous alloc_site bits in the higher-level 
	 * chunk. But we assume that's been/being deleted, so we don't bother. */
#endif
}

/* The (unsigned) -1 conversion here provokes a compiler warning,
 * which we suppress. There are two ways of doing this.
 * One is to turn the warning off and back on again, clobbering the former setting.
 * Another is, if the GCC version we have allows it (must be > 4.6ish),
 * to use the push/pop mechanism. If we can't pop, we leave it "on" (conservative).
 * To handle the case where we don't have push/pop, 
 * we also suppress pragma warnings, then re-enable them. :-) */
#pragma GCC diagnostic ignored "-Wpragmas"
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Woverflow"
static void check_impl_sanity(void)
{
	assert(PAGE_SIZE == sysconf(_SC_PAGE_SIZE));
	assert(LOG_PAGE_SIZE == integer_log2(PAGE_SIZE));

	assert(sizeof (struct entry) == 1);
	assert(
			entry_to_offset((struct entry){ .present = 1, .removed = 0, .distance = (unsigned) -1})
			+ entry_to_offset((struct entry){ .present = 1, .removed = 0, .distance = 1 }) 
		== entry_coverage_in_bytes);
}
/* First, re-enable the overflow pragma, to be conservative. */
#pragma GCC diagnostic warning "-Woverflow"
/* Now, if we have "pop", we will restore it to its actual former setting. */
#pragma GCC diagnostic pop
#pragma GCC diagnostic warning "-Wpragmas"

static _Bool tried_to_init;

static void
do_init(void)
{
	/* Optionally delay, for attaching a debugger. */
	if (getenv("HEAP_INDEX_DELAY_INIT")) sleep(8);

	/* Check we got the shift logic correct in entry_to_offset, and other compile-time logic. */
	check_impl_sanity();
	
	/* If we're already trying to initialize, or have already
	 * tried, don't try recursively/again. */
	if (tried_to_init) return;
	tried_to_init = 1;
	
	if (index_region) return; /* already done */

	/* Initialize what we depend on. */
	__sbrk_allocator_init();
	__mmap_allocator_init();
	
	index_begin_addr = (void*) 0U;
#if defined(__x86_64__) || defined(x86_64)
	index_end_addr = (void*)(1ULL<<47); /* it's effectively a 47-bit address space */
#else
	index_end_addr = (void*) 0U; /* both 0 => cover full address range */
#endif
	
	size_t mapping_size = MEMTABLE_MAPPING_SIZE_WITH_TYPE(struct entry,
		entry_coverage_in_bytes, 
		index_begin_addr,
		index_end_addr
	);

	if (mapping_size > BIGGEST_MMAP_ALLOWED)
	{
#ifndef NDEBUG
		fprintf(stderr, "%s: warning: mapping %lld bytes not %ld\n",
			__FILE__, BIGGEST_MMAP_ALLOWED, mapping_size);
		fprintf(stderr, "%s: warning: only bottom 1/%lld of address space is tracked.\n",
			__FILE__, mapping_size / BIGGEST_MMAP_ALLOWED);
#endif
		mapping_size = BIGGEST_MMAP_ALLOWED;
		/* Back-calculate what address range we can cover from this mapping size. */
		unsigned long long nentries = mapping_size / sizeof (entry_type);
		void *one_past_max_indexed_address = index_begin_addr +
			nentries * entry_coverage_in_bytes;
		index_end_addr = one_past_max_indexed_address;
	}
	
	index_region = MEMTABLE_NEW_WITH_TYPE(struct entry, 
		entry_coverage_in_bytes, index_begin_addr, index_end_addr);
	debug_printf(3, "heap_index at %p\n", index_region);
	
	assert(index_region != MAP_FAILED);
}

void post_init(void) __attribute__((visibility("hidden")));
void post_init(void)
{
	do_init();
}

static inline struct insert *insert_for_chunk(void *userptr);

#ifndef NDEBUG
/* In this newer, more space-compact implementation, we can't do as much
 * sanity checking. Check that if our entry is not present, our distance
 * is 0. */
#define INSERT_SANITY_CHECK(p_t) assert( \
	!(!((p_t)->un.ptrs.next.present) && !((p_t)->un.ptrs.next.removed) && (p_t)->un.ptrs.next.distance != 0) \
	&& !(!((p_t)->un.ptrs.prev.present) && !((p_t)->un.ptrs.prev.removed) && (p_t)->un.ptrs.prev.distance != 0))

static void list_sanity_check(entry_type *head, const void *should_see_chunk)
{
	void *head_chunk = entry_ptr_to_addr(head);
	_Bool saw_should_see_chunk = 0;
#ifdef TRACE_HEAP_INDEX
	fprintf(stderr,
		"Begin sanity check of list indexed at %p, head chunk %p\n",
		head, head_chunk);
#endif
	void *cur_userchunk = head_chunk;
	unsigned count = 0;
	while (cur_userchunk != NULL)
	{
		++count;
		if (should_see_chunk && cur_userchunk == should_see_chunk) saw_should_see_chunk = 1;
		INSERT_SANITY_CHECK(insert_for_chunk(cur_userchunk));
		/* If the next chunk link is null, entry_to_same_range_addr
		 * should detect this (.present == 0) and give us NULL. */
		void *next_userchunk
		 = entry_to_same_range_addr(
			insert_for_chunk(cur_userchunk)->un.ptrs.next, 
			cur_userchunk
		);
#ifdef TRACE_HEAP_INDEX
		fprintf(stderr, "List has a chunk beginning at userptr %p"
			" (usable_size %zu, insert {next: %p, prev %p})\n",
			cur_userchunk, 
			malloc_usable_size(userptr_to_allocptr(cur_userchunk)),
			next_userchunk,
			entry_to_same_range_addr(
				insert_for_chunk(cur_userchunk)->un.ptrs.prev, 
				cur_userchunk
			)
		);
#endif
		assert(next_userchunk != head_chunk);
		assert(next_userchunk != cur_userchunk);

		/* If we're not the first element, we should have a 
		 * prev chunk. */
		if (count > 1) assert(NULL != entry_to_same_range_addr(
				insert_for_chunk(cur_userchunk)->un.ptrs.prev, 
				cur_userchunk
			));


		cur_userchunk = next_userchunk;
	}
	if (should_see_chunk && !saw_should_see_chunk)
	{
#ifdef TRACE_HEAP_INDEX
		fprintf(stderr, "Was expecting to find chunk at %p\n", should_see_chunk);
#endif

	}
	assert(!should_see_chunk || saw_should_see_chunk);
#ifdef TRACE_HEAP_INDEX
	fprintf(stderr,
		"Passed sanity check of list indexed at %p, head chunk %p, "
		"length %d\n", head, head_chunk, count);
#endif
}
#else /* NDEBUG */
#define INSERT_SANITY_CHECK(p_t)
static void list_sanity_check(entry_type *head, const void *should_see_chunk) {}
#endif

static uintptr_t nbytes_in_index_for_bigalloc_entry(void *userchunk_base)
{
	void *allocptr = userptr_to_allocptr(userchunk_base);
	void *end_addr = (char*) allocptr + malloc_usable_size(allocptr);
	uintptr_t begin_pagenum = ((uintptr_t) userchunk_base >> LOG_PAGE_SIZE);
	uintptr_t end_pagenum = ((uintptr_t) end_addr >> LOG_PAGE_SIZE)
			 + (((((uintptr_t) end_addr) % PAGE_SIZE) == 0) ? 0 : 1);
	unsigned long nbytes_in_index = ((end_pagenum - begin_pagenum) << LOG_PAGE_SIZE)
			/ entry_coverage_in_bytes;
	return nbytes_in_index;
}

static void 
index_insert(void *new_userchunkaddr, size_t modified_size, const void *caller);

void 
__liballocs_index_insert(void *new_userchunkaddr, size_t modified_size, const void *caller)
{
	index_insert(new_userchunkaddr, modified_size, caller);
}

static void 
index_insert(void *new_userchunkaddr, size_t modified_size, const void *caller)
{
	int lock_ret;
	BIG_LOCK
	
	/* We *must* have been initialized to continue. So initialize now.
	 * (Sometimes the initialize hook doesn't get called til after we are called.) */
	if (!index_region) do_init();
	assert(index_region);
	
	/* The address *must* be in our tracked range. Assert this. */
	assert(new_userchunkaddr <= (index_end_addr ? index_end_addr : MAP_FAILED));
	
#ifdef TRACE_HEAP_INDEX
	/* Check the recently freed list for this pointer. Delete it if we find it. */
	for (int i = 0; i < RECENTLY_FREED_SIZE; ++i)
	{
		if (recently_freed[i] == new_userchunkaddr)
		{ 
			recently_freed[i] = NULL;
			next_recently_freed_to_replace = &recently_freed[i];
		}
	}
#endif
	
	/* If we're big enough, 
	 * push our metadata into the bigalloc map. 
	 * (Do we still index it at l1? NO, but this stores up complication when we need to promote it.  */
	if (__builtin_expect(
			modified_size > /* HACK: default glibc lower mmap threshold: 128 kB */ 131072
			/* NOTE: no longer do we have to be page-aligned to use the bigalloc map */
			/* && (uintptr_t) userptr_to_allocptr(new_userchunkaddr) % PAGE_SIZE <= MAXIMUM_MALLOC_HEADER_OVERHEAD */
			, 
		0))
	{
		const struct big_allocation *b = __liballocs_new_bigalloc(
			userptr_to_allocptr(new_userchunkaddr),
			modified_size - sizeof (struct insert),
			(struct meta_info) {
				.what = INS_AND_BITS,
				.un = {
					ins_and_bits: { 
						.ins = (struct insert) {
							.alloc_site_flag = 0,
							.alloc_site = (uintptr_t) caller
						}
					}
				}
			},
			NULL,
			&__generic_malloc_allocator
		);
		if (b)
		{
			/* memset the covered entries with the bigalloc value,
			 * to tell our lookup code that it should ask the page index. */
			struct entry bigalloc_value = { 0, 1, 63 };
			assert(IS_BIGALLOC_ENTRY(&bigalloc_value));
			memset(INDEX_LOC_FOR_ADDR(new_userchunkaddr), *(char*) &bigalloc_value, 
				nbytes_in_index_for_bigalloc_entry(new_userchunkaddr));
			assert(IS_BIGALLOC_ENTRY(INDEX_LOC_FOR_ADDR(new_userchunkaddr)));
			assert(IS_BIGALLOC_ENTRY(INDEX_LOC_FOR_ADDR((char*) new_userchunkaddr + modified_size - 1)));
			
			BIG_UNLOCK
			return;
		}
	}
	
	/* if we got here, it's going in l1 */
	if (modified_size > biggest_l1_object) biggest_l1_object = modified_size;

	struct entry *index_entry = INDEX_LOC_FOR_ADDR(new_userchunkaddr);

	/* DEBUGGING: sanity check entire bin */
#ifdef TRACE_HEAP_INDEX
	fprintf(stderr, "*** Inserting user chunk at %p into list indexed at %p\n", 
		new_userchunkaddr, index_entry);
#endif
	list_sanity_check(index_entry, NULL);

	void *head_chunkptr = entry_ptr_to_addr(index_entry);
	
	/* Populate our extra fields */
	struct insert *p_insert = insert_for_chunk(new_userchunkaddr);
	p_insert->alloc_site_flag = 0U;
	p_insert->alloc_site = (uintptr_t) caller;

	/* Add it to the index. We always add to the start of the list, for now. */
	/* 1. Initialize our insert. */
	p_insert->un.ptrs.next = addr_to_entry(head_chunkptr);
	p_insert->un.ptrs.prev = addr_to_entry(NULL);
	assert(!p_insert->un.ptrs.prev.present);
	
	/* 2. Fix up the next insert, if there is one */
	if (p_insert->un.ptrs.next.present)
	{
		insert_for_chunk(entry_to_same_range_addr(p_insert->un.ptrs.next, new_userchunkaddr))->un.ptrs.prev
		 = addr_to_entry(new_userchunkaddr);
	}
	/* 3. Fix up the index. */
	*index_entry = addr_to_entry(new_userchunkaddr); // FIXME: thread-safety

	/* sanity checks */
	struct entry *e = index_entry;
	assert(e->present); // it's there
	assert(insert_for_chunk(entry_ptr_to_addr(e)));
	assert(insert_for_chunk(entry_ptr_to_addr(e)) == p_insert);
	INSERT_SANITY_CHECK(p_insert);
	if (p_insert->un.ptrs.next.present) INSERT_SANITY_CHECK(
		insert_for_chunk(entry_to_same_range_addr(p_insert->un.ptrs.next, new_userchunkaddr)));
	if (p_insert->un.ptrs.prev.present) INSERT_SANITY_CHECK(
		insert_for_chunk(entry_to_same_range_addr(p_insert->un.ptrs.prev, new_userchunkaddr)));
	list_sanity_check(e, new_userchunkaddr);
	
	BIG_UNLOCK
}

void 
post_successful_alloc(void *allocptr, size_t modified_size, size_t modified_alignment, 
		size_t requested_size, size_t requested_alignment, const void *caller)
		__attribute__((visibility("hidden")));
void 
post_successful_alloc(void *allocptr, size_t modified_size, size_t modified_alignment, 
		size_t requested_size, size_t requested_alignment, const void *caller)
{
	index_insert(allocptr /* == userptr */, modified_size, __current_allocsite ? __current_allocsite : caller);
	safe_to_call_malloc = 1; // if somebody succeeded, anyone should succeed
}

void pre_alloc(size_t *p_size, size_t *p_alignment, const void *caller) __attribute__((visibility("hidden")));
void pre_alloc(size_t *p_size, size_t *p_alignment, const void *caller)
{
	/* We increase the size by the amount of extra data we store, 
	 * and possibly a bit more to allow for alignment.  */
	size_t orig_size = *p_size;
	/* Add the size of struct insert, and round this up to the align of struct insert. 
	 * This ensure we always have room for an *aligned* struct insert. */
	size_t size_with_insert = orig_size + sizeof (struct insert);
	size_t size_to_allocate = PAD_TO_ALIGN(size_with_insert, sizeof (struct insert));
	assert(0 == size_to_allocate % ALIGNOF(struct insert));
	*p_size = size_to_allocate;
}

struct insert *__liballocs_insert_for_chunk_and_usable_size(void *userptr, size_t usable_size)
{
	return insert_for_chunk_and_usable_size(userptr, usable_size);
}

static void index_delete(void *userptr);

void 
__liballocs_index_delete(void *userptr)
{
	index_delete(userptr);
}

static void index_delete(void *userptr/*, size_t freed_usable_size*/)
{
	/* The freed_usable_size is not strictly necessary. It was added
	 * for handling realloc after-the-fact. In this case, by the time we
	 * get called, the usable size has already changed. However, after-the-fact
	 * was a broken way to handle realloc() when we were using trailers instead
	 * of inserts, because in the case of a *smaller*
	 * realloc'd size, where the realloc happens in-place, realloc() would overwrite
	 * our insert with its own (regular heap metadata) trailer, breaking the list.
	 */
	
	if (userptr == NULL) return; // HACK: shouldn't be necessary; a BUG somewhere
	
	/* HACK for libcrunch cache invalidation */
	if (__libcrunch_uncache_all)
	{
		void *allocptr = userptr_to_allocptr(userptr);
		__libcrunch_uncache_all(allocptr, malloc_usable_size(allocptr));
	}
	
	int lock_ret;
	BIG_LOCK
	
#ifdef TRACE_HEAP_INDEX
	/* Check the recently-freed list for this pointer. We will warn about
	 * a double-free if we hit it. */
	for (int i = 0; i < RECENTLY_FREED_SIZE; ++i)
	{
		if (recently_freed[i] == userptr)
		{
			fprintf(stderr, "*** Double free detected for alloc chunk %p\n", 
				userptr);
			return;
		}
	}
#endif
	
	struct entry *index_entry = INDEX_LOC_FOR_ADDR(userptr);
	/* unindex the bigalloc maps */
	if (__builtin_expect(IS_BIGALLOC_ENTRY(index_entry), 0))
	{
		void *allocptr = userptr_to_allocptr(userptr);
		unsigned long size = malloc_usable_size(allocptr);
#ifdef TRACE_HEAP_INDEX
		fprintf(stderr, "*** Unindexing bigalloc entry for alloc chunk %p (size %lu)\n", 
				allocptr, size);
#endif
		unsigned start_remainder = ((uintptr_t) allocptr) % PAGE_SIZE;
		unsigned end_remainder = (((uintptr_t) allocptr) + size) % PAGE_SIZE;
		
		unsigned expected_pagewise_size = size 
				+ start_remainder
				+ ((end_remainder == 0) ? 0 : PAGE_SIZE - end_remainder);
		__liballocs_delete_bigalloc(userptr_to_allocptr(userptr), 
			&__generic_malloc_allocator);
		// memset the covered entries with the empty value
		struct entry empty_value = { 0, 0, 0 };
		assert(IS_EMPTY_ENTRY(&empty_value));
		memset(index_entry, *(char*) &empty_value, nbytes_in_index_for_bigalloc_entry(userptr));
		
#ifdef TRACE_HEAP_INDEX
		*next_recently_freed_to_replace = userptr;
		++next_recently_freed_to_replace;
		if (next_recently_freed_to_replace == &recently_freed[RECENTLY_FREED_SIZE])
		{
			next_recently_freed_to_replace = &recently_freed[0];
		}
#endif
		
		BIG_UNLOCK
		return;
	}

#ifdef TRACE_HEAP_INDEX
	fprintf(stderr, "*** Deleting entry for chunk %p, from list indexed at %p\n", 
		userptr, index_entry);
#endif
	
	unsigned suballocated_region_number = 0;
	struct insert *ins = insert_for_chunk(userptr);
	if (ALLOC_IS_SUBALLOCATED(userptr, ins)) 
	{
		suballocated_region_number = (uintptr_t) ins->alloc_site;
	}

	list_sanity_check(index_entry, userptr);
	INSERT_SANITY_CHECK(insert_for_chunk/*_with_usable_size*/(userptr/*, freed_usable_size*/));

	/* (old comment; still true?) FIXME: we need a big lock around realloc()
	 * to avoid concurrent in-place realloc()s messing with the other inserts we access. */

	/* remove it from the bins */
	void *our_next_chunk = entry_to_same_range_addr(insert_for_chunk(userptr)->un.ptrs.next, userptr);
	void *our_prev_chunk = entry_to_same_range_addr(insert_for_chunk(userptr)->un.ptrs.prev, userptr);
	
	/* FIXME: make these atomic */
	if (our_prev_chunk) 
	{
		INSERT_SANITY_CHECK(insert_for_chunk(our_prev_chunk));
		insert_for_chunk(our_prev_chunk)->un.ptrs.next = addr_to_entry(our_next_chunk);
	}
	else /* !our_prev_chunk */
	{
		/* removing head of the list */
		*index_entry = addr_to_entry(our_next_chunk);
		if (!our_next_chunk)
		{
			/* ... it's a singleton list, so 
			 * - no prev chunk to update
			 * - the index entry should be non-present
			 * - exit */
			assert(index_entry->present == 0);
			goto out;
		}
	}

	if (our_next_chunk) 
	{
		INSERT_SANITY_CHECK(insert_for_chunk(our_next_chunk));
		
		/* may assign NULL here, if we're removing the head of the list */
		insert_for_chunk(our_next_chunk)->un.ptrs.prev = addr_to_entry(our_prev_chunk);
	}
	else /* !our_next_chunk */
	{
		/* removing tail of the list... */
		/* ... and NOT a singleton -- we've handled that case already */
		assert(our_prev_chunk);
	
		/* update the previous chunk's insert */
		insert_for_chunk(our_prev_chunk)->un.ptrs.next = addr_to_entry(NULL);

		/* nothing else to do here, as we don't keep a tail pointer */
	}
	/* Now that we have deleted the record, our bin should be sane,
	 * modulo concurrent reallocs. */
out:
#ifdef TRACE_HEAP_INDEX
	*next_recently_freed_to_replace = userptr;
	++next_recently_freed_to_replace;
	if (next_recently_freed_to_replace == &recently_freed[RECENTLY_FREED_SIZE])
	{
		next_recently_freed_to_replace = &recently_freed[0];
	}
#endif
	/* If there were suballocated chunks under here, delete the whole lot. */
	if (suballocated_region_number != 0)
	{
		delete_suballocated_chunk(&suballocated_chunks[suballocated_region_number]);
	}
	invalidate_cache_entries(userptr, (unsigned short) -1, NULL, NULL, -1);
	list_sanity_check(index_entry, NULL);
	
	BIG_UNLOCK
}

void pre_nonnull_free(void *userptr, size_t freed_usable_size) __attribute__((visibility("hidden")));
void pre_nonnull_free(void *userptr, size_t freed_usable_size)
{
	index_delete(userptr/*, freed_usable_size*/);
}

void post_nonnull_free(void *userptr) __attribute__((visibility("hidden")));
void post_nonnull_free(void *userptr) 
{}

void pre_nonnull_nonzero_realloc(void *userptr, size_t size, const void *caller) __attribute__((visibility("hidden")));
void pre_nonnull_nonzero_realloc(void *userptr, size_t size, const void *caller)
{
	/* When this happens, we *may or may not be freeing an area*
	 * -- i.e. if the realloc fails, we will not actually free anything.
	 * However, whn we were using trailers, and 
	 * in the case of realloc()ing a *slightly smaller* region, 
	 * the allocator might trash our insert (by writing its own data over it). 
	 * So we *must* delete the entry first,
	 * then recreate it later, as it may not survive the realloc() uncorrupted. */
	index_delete(userptr/*, malloc_usable_size(ptr)*/);
}
void post_nonnull_nonzero_realloc(void *userptr, 
	size_t modified_size, 
	size_t old_usable_size,
	const void *caller, void *__new_allocptr)
{
	if (__new_allocptr != NULL)
	{
		/* create a new bin entry */
		index_insert(allocptr_to_userptr(__new_allocptr), 
				modified_size, __current_allocsite ? __current_allocsite : caller);
	}
	else 
	{
		/* *recreate* the old bin entry! The old usable size
		 * is the *modified* size, i.e. we modified it before
		 * allocating it, so we pass it as the modified_size to
		 * index_insert. */
		index_insert(userptr, old_usable_size, __current_allocsite ? __current_allocsite : caller);
	} 
}

// same but zero bytes, not bits
static int nlzb1(unsigned long x) {
	int n;

	if (x == 0) return 8;
	n = 0;

	if (x <= 0x00000000FFFFFFFFL) { n += 4; x <<= 32; }
	if (x <= 0x0000FFFFFFFFFFFFL) { n += 2; x <<= 16; }
	if (x <= 0x00FFFFFFFFFFFFFFL) { n += 1;  x <<= 8; }
	
	return n;
}

static inline unsigned char *rfind_nonzero_byte(unsigned char *one_beyond_start, unsigned char *last_good_byte)
{
#define SIZE (sizeof (unsigned long))
#define IS_ALIGNED(p) (((uintptr_t)(p)) % (SIZE) == 0)

	unsigned char *p = one_beyond_start;
	/* Do the unaligned part */
	while (!IS_ALIGNED(p))
	{
		--p;
		if (p < last_good_byte) return NULL;
		if (*p != 0) return p;
	}
	// now p is aligned and any address >=p is not the one we want
	// (if we had an aligned pointer come in, we don't want it -- it's one_beyond_start)

	/* Do the aligned part. */
	while (p-SIZE >= last_good_byte)
	{
		p -= SIZE;
		unsigned long v = *((unsigned long *) p);
		if (v != 0ul)
		{
			// HIT -- but what is the highest nonzero byte?
			int nlzb = nlzb1(v); // in range 0..7
			return p + SIZE - 1 - nlzb;
		}
	}
	// now we have tested all bytes from p upwards
	// and p-SIZE < last_good_byte
	long nbytes_remaining = p - last_good_byte;
	assert(nbytes_remaining < SIZE);
	assert(nbytes_remaining >= 0);
	
	/* Do the unaligned part */
	while (p > last_good_byte)
	{
		--p;
		if (*p != 0) return p;
	}
	
	return NULL;
#undef IS_ALIGNED
#undef SIZE
}

static inline _Bool find_next_nonempty_bin(struct entry **p_cur, 
		struct entry *limit,
		size_t *p_object_minimum_size
		)
{
	size_t max_nbytes_coverage_to_scan = biggest_l1_object - *p_object_minimum_size;
	size_t max_nbuckets_to_scan = 
			(max_nbytes_coverage_to_scan % entry_coverage_in_bytes) == 0 
		?    max_nbytes_coverage_to_scan / entry_coverage_in_bytes
		:    (max_nbytes_coverage_to_scan / entry_coverage_in_bytes) + 1;
	unsigned char *limit_by_size = (unsigned char *) *p_cur - max_nbuckets_to_scan;
	unsigned char *limit_to_pass = (limit_by_size > (unsigned char *) index_region)
			 ? limit_by_size : (unsigned char *) index_region;
	unsigned char *found = rfind_nonzero_byte((unsigned char *) *p_cur, limit_to_pass);
	if (!found) 
	{ 
		*p_object_minimum_size += (((unsigned char *) *p_cur) - limit_to_pass) * entry_coverage_in_bytes; 
		*p_cur = (struct entry *) limit_to_pass;
		return 0;
	}
	else
	{ 
		*p_object_minimum_size += (((unsigned char *) *p_cur) - found) * entry_coverage_in_bytes; 
		*p_cur = (struct entry *) found; 
		return 1;
	}

	// FIXME: adapt http://www.int80h.org/strlen/ 
	// or memrchr.S from eglibc
	// to do what we want.
}

#ifndef LOOKUP_CACHE_SIZE
#define LOOKUP_CACHE_SIZE 4
#endif
struct lookup_cache_entry
{
	void *object_start;
	size_t usable_size:60;
	unsigned short depth:3;
	unsigned short is_deepest:1;
	struct suballocated_chunk_rec *containing_chunk;
	struct insert *insert;
} lookup_cache[LOOKUP_CACHE_SIZE];
static struct lookup_cache_entry *next_to_evict = &lookup_cache[0];

static void check_cache_sanity(void)
{
#ifndef NDEBUG
	for (int i = 0; i < LOOKUP_CACHE_SIZE; ++i)
	{
		assert(!lookup_cache[i].object_start 
				|| (INSERT_DESCRIBES_OBJECT(lookup_cache[i].insert)
					&& lookup_cache[i].depth <= 2));
	}
#endif
}

static void install_cache_entry(void *object_start,
	size_t object_size,
	unsigned short depth, 
	_Bool is_deepest,
	struct suballocated_chunk_rec *containing_chunk,
	struct insert *insert)
{
	check_cache_sanity();
	/* our "insert" should always be the insert that describes the object,
	 * NOT one that chains into the suballocs table. */
	assert(INSERT_DESCRIBES_OBJECT(insert));
	assert(next_to_evict >= &lookup_cache[0] && next_to_evict < &lookup_cache[LOOKUP_CACHE_SIZE]);
	*next_to_evict = (struct lookup_cache_entry) {
		object_start, object_size, depth, is_deepest, containing_chunk, insert
	}; // FIXME: thread safety
	// don't immediately evict the entry we just created
	next_to_evict = &lookup_cache[(next_to_evict + 1 - &lookup_cache[0]) % LOOKUP_CACHE_SIZE];
	assert(next_to_evict >= &lookup_cache[0] && next_to_evict < &lookup_cache[LOOKUP_CACHE_SIZE]);
	check_cache_sanity();
}

static void invalidate_cache_entries(void *object_start,
	unsigned short depths_mask,
	struct suballocated_chunk_rec *containing,
	struct insert *ins,
	signed nentries)
{
	unsigned ninvalidated = 0;
	check_cache_sanity();
	for (unsigned i = 0; i < LOOKUP_CACHE_SIZE; ++i)
	{
		if ((!object_start || object_start == lookup_cache[i].object_start)
				&& (!containing || containing == lookup_cache[i].containing_chunk)
				&& (!ins || ins == lookup_cache[i].insert)
				&& (0 != (1<<lookup_cache[i].depth & depths_mask))) 
		{
			lookup_cache[i] = (struct lookup_cache_entry) {
				NULL, 0, 0, 0, NULL, NULL
			};
			next_to_evict = &lookup_cache[i];
			check_cache_sanity();
			++ninvalidated;
			if (nentries > 0 && ninvalidated >= nentries) return;
		}
	}
	check_cache_sanity();
}

static int cache_clear_deepest_flag_and_update_ins(void *object_start,
	unsigned short depths_mask,
	struct suballocated_chunk_rec *containing,
	struct insert *ins,
	signed nentries,
	struct insert *new_ins)
{
	unsigned ncleared = 0;
	// we might be used to restore the cache invariant, so don't check
	// check_cache_sanity();
	assert(ins);
	for (unsigned i = 0; i < LOOKUP_CACHE_SIZE; ++i)
	{
		if ((!object_start || object_start == lookup_cache[i].object_start)
				&& (!containing || containing == lookup_cache[i].containing_chunk)
				&& (ins == lookup_cache[i].insert)
				&& (0 != (1<<lookup_cache[i].depth & depths_mask))) 
		{
			lookup_cache[i].is_deepest = 0;
			lookup_cache[i].insert = new_ins;
			check_cache_sanity();
			++ncleared;
			if (nentries > 0 && ncleared >= nentries) return ncleared;
		}
	}
	check_cache_sanity();
	return ncleared;
}

static
struct insert *lookup_l01_object_info(const void *mem, void **out_object_start);
static
struct insert *lookup_l01_object_info_nocache(const void *mem, void **out_object_start);

static 
struct insert *object_insert(const void *obj, struct insert *ins)
{
	if (__builtin_expect(!INSERT_DESCRIBES_OBJECT(ins), 0))
	{
		struct suballocated_chunk_rec *p_rec = &suballocated_chunks[(unsigned) ins->alloc_site];
		assert(p_rec);
		return &p_rec->higherlevel_ins; // FIXME: generalise to depth > 2
	}
	return ins;
}

/* A client-friendly lookup function with cache. */
struct insert *lookup_object_info(const void *mem, void **out_object_start, size_t *out_object_size,
		struct suballocated_chunk_rec **out_containing_chunk)
{
	/* Unlike our malloc hooks, we might get called before initialization,
	   e.g. if someone tries to do a lookup before the first malloc of the
	   program's execution. Rather than putting an initialization check
	   in the fast-path functions, we bail here.  */
	if (!index_region) return NULL;
	
	/* Try matching in the cache. NOTE: how does this impact bigalloc and deep-indexed 
	 * entries? In all cases, we cache them here. We also keep a "is_deepest" flag
	 * which tells us (conservatively) whether it's known to be the deepest entry
	 * indexing that storage. In this function, we *only* return a cache hit if the 
	 * flag is set. (In lookup_l01_object_info, this logic is different.) */
	check_cache_sanity();
	void *l01_object_start = NULL;
	struct insert *found_l01 = NULL;
	for (unsigned i = 0; i < LOOKUP_CACHE_SIZE; ++i)
	{
		if (lookup_cache[i].object_start && 
				(char*) mem >= (char*) lookup_cache[i].object_start && 
				(char*) mem < (char*) lookup_cache[i].object_start + lookup_cache[i].usable_size)
		{
			// possible hit
			if (lookup_cache[i].depth == 1 || lookup_cache[i].depth == 0)
			{
				l01_object_start = lookup_cache[i].object_start;
				found_l01 = lookup_cache[i].insert;
			}
			
			if (lookup_cache[i].is_deepest)
			{
				// HIT!
				assert(lookup_cache[i].object_start);
	#if defined(TRACE_DEEP_HEAP_INDEX) || defined(TRACE_HEAP_INDEX)
				fprintf(stderr, "Cache hit at pos %d (%p) with alloc site %p\n", i, 
						lookup_cache[i].object_start, (void*) (uintptr_t) lookup_cache[i].insert->alloc_site);
				fflush(stderr);
	#endif
				assert(INSERT_DESCRIBES_OBJECT(lookup_cache[i].insert));

				if (out_object_start) *out_object_start = lookup_cache[i].object_start;
				if (out_object_size) *out_object_size = lookup_cache[i].usable_size;
				if (out_containing_chunk) *out_containing_chunk = lookup_cache[i].containing_chunk;
				// ... so ensure we're not about to evict this guy
				if (next_to_evict - &lookup_cache[0] == i)
				{
					next_to_evict = &lookup_cache[(i + 1) % LOOKUP_CACHE_SIZE];
					assert(next_to_evict - &lookup_cache[0] < LOOKUP_CACHE_SIZE);
				}
				assert(INSERT_DESCRIBES_OBJECT(lookup_cache[i].insert));
				return lookup_cache[i].insert;
			}
		}
	}
	
	// didn't hit cache, but we may have seen the l01 entry
	struct insert *found;
	void *object_start;
	unsigned short depth = 1;
	if (found_l01)
	{
		/* CARE: the cache's p_ins points to the alloc's insert, even if it's been
		 * moved (in the suballocated case). So we re-lookup the physical insert here. */
		found = insert_for_chunk(l01_object_start);
	}
	else
	{
		found = lookup_l01_object_info_nocache(mem, &l01_object_start);
	}
	size_t size;
	struct suballocated_chunk_rec *containing_chunk_rec = NULL; // initialized shortly...

	if (found)
	{
		size = usersize(l01_object_start);
		object_start = l01_object_start;
		containing_chunk_rec = NULL;
		_Bool is_deepest = INSERT_DESCRIBES_OBJECT(found);
		
		// cache the l01 entry
		install_cache_entry(object_start, size, 1, is_deepest, NULL, object_insert(object_start, found));
		
		if (!is_deepest)
		{
			assert(l01_object_start);
			/* deep case */
			void *deep_object_start;
			size_t deep_object_size;
			struct insert *found_deeper = NULL; /*lookup_deep_alloc(mem, 1, found, &deep_object_start, 
					&deep_object_size, &containing_chunk_rec);*/
			if (found_deeper)
			{
				// override the values we assigned just now
				object_start = deep_object_start;
				found = found_deeper;
				size = deep_object_size;
				// cache this too
				install_cache_entry(object_start, size, 2 /* FIXME */, 1 /* FIXME */, 
					containing_chunk_rec, found);
			}
			else
			{
				// we still have to point the metadata at the *sub*indexed copy
				assert(!INSERT_DESCRIBES_OBJECT(found));
				found = object_insert(mem, found);
			}
		}


		if (out_object_start) *out_object_start = object_start;
		if (out_object_size) *out_object_size = size;
		if (out_containing_chunk) *out_containing_chunk = containing_chunk_rec;
	}
	
	assert(!found || INSERT_DESCRIBES_OBJECT(found));
	return found;
}

static
struct insert *lookup_l01_object_info(const void *mem, void **out_object_start) 
{
	// first, try the cache
	check_cache_sanity();
	for (unsigned i = 0; i < LOOKUP_CACHE_SIZE; ++i)
	{
		if (lookup_cache[i].object_start && 
				lookup_cache[i].depth <= 1 && 
				(char*) mem >= (char*) lookup_cache[i].object_start && 
				(char*) mem < (char*) lookup_cache[i].object_start + lookup_cache[i].usable_size)
		{
			// HIT!
			struct insert *real_ins = object_insert(lookup_cache[i].object_start, lookup_cache[i].insert);
#if defined(TRACE_DEEP_HEAP_INDEX) || defined(TRACE_HEAP_INDEX)
			fprintf(stderr, "Cache[l01] hit at pos %d (%p) with alloc site %p\n", i, 
					lookup_cache[i].object_start, (void*) (uintptr_t) real_ins->alloc_site);
			fflush(stderr);
#endif
			assert(INSERT_DESCRIBES_OBJECT(real_ins));
			
			if (out_object_start) *out_object_start = lookup_cache[i].object_start;

			// ... so ensure we're not about to evict this guy
			if (next_to_evict - &lookup_cache[0] == i)
			{
				next_to_evict = &lookup_cache[(i + 1) % LOOKUP_CACHE_SIZE];
				assert(next_to_evict - &lookup_cache[0] < LOOKUP_CACHE_SIZE);
			}
			// return the possibly-SUBALLOC insert -- not the one from the cache
			return insert_for_chunk(lookup_cache[i].object_start);
		}
	}
	
	return lookup_l01_object_info_nocache(mem, out_object_start);
}

static
struct insert *lookup_l01_object_info_nocache(const void *mem, void **out_object_start) 
{
	struct entry *first_head = INDEX_LOC_FOR_ADDR(mem);
	struct entry *cur_head = first_head;
	size_t object_minimum_size = 0;

	// Optimisation: if we see an object
	// in the current bucket that starts before our object, 
	// but doesn't span the address we're searching for,
	// we don't need to look at previous buckets, 
	// because we know that our pointer can't be an interior
	// pointer into some object starting in a earlier bucket's region.
	_Bool seen_object_starting_earlier = 0;
	do
	{
		seen_object_starting_earlier = 0;
		
		if (__builtin_expect(IS_BIGALLOC_ENTRY(cur_head), 0))
		{
			return __lookup_bigalloc_with_insert(mem, &__generic_malloc_allocator, out_object_start);
		}
	
// 		if (__builtin_expect(IS_DEEP_ENTRY(cur_head), 0))
// 		{
// 			if (cur_head != first_head)
// 			{
// 				/* If we didn't point into non-deep-indexed memory at ptr,
// 				 * we're not going to find our allocation in a deep-indexed
// 				 * region, since we always upgrade units of at least a whole
// 				 * chunk. So we can abort now. 
// 				 */
// #ifdef TRACE_DEEP_HEAP_INDEX
// 				fprintf(stderr, "Strayed into deep-indexed region (bucket base %p) "
// 						"searching for chunk overlapping %p, so aborting\n", 
// 					ADDR_FOR_INDEX_LOC(cur_head), mem);
// #endif
// 				goto fail;
// 			}
// 			struct deep_entry_region *region;
// 			struct deep_entry *found = lookup_deep_alloc((void*) mem, /*cur_head,*/ -1, -1, &region, &seen_object_starting_earlier);
// 			// did we find an overlapping object?
// 			if (found)
// 			{
// 				if (out_deep) *out_deep = found;
// 				void *object_start = (char*) region->base_addr + (found->distance_4bytes << 2);
// 				if (out_object_start) *out_object_start = object_start;
// 				install_cache_entry(object_start, (found->size_4bytes << 2), found, &found->u_tail.ins);
// 				return &found->u_tail.ins;
// 			}
// 			else
// 			{
// 				// we should at least have a region
// 				assert(region);
// 				// resume the search from the next-lower index
// 				cur_head = INDEX_LOC_FOR_ADDR((char*) region->base_addr - 1);
// 				continue;
// 			}
// 		}
		
		void *cur_userchunk = entry_ptr_to_addr(cur_head);

		while (cur_userchunk)
		{
			struct insert *cur_insert = insert_for_chunk(cur_userchunk);
#ifndef NDEBUG
			/* Sanity check on the insert. */
			if ((char*) cur_insert < (char*) cur_userchunk
				|| (char*) cur_insert - (char*) cur_userchunk > biggest_l1_object)
			{
				fprintf(stderr, "Saw insane insert address %p for chunk beginning %p "
					"(usable size %zu, allocptr %p); memory corruption?\n", 
					cur_insert, cur_userchunk, 
					malloc_usable_size(userptr_to_allocptr(cur_userchunk)), 
					userptr_to_allocptr(cur_userchunk));
			}	
#endif
			if (mem >= cur_userchunk
				&& mem < cur_userchunk + malloc_usable_size(userptr_to_allocptr(cur_userchunk))) 
			{
				// match!
				if (out_object_start) *out_object_start = cur_userchunk;
				return cur_insert;
			}
			
			// do that optimisation
			if (cur_userchunk < mem) seen_object_starting_earlier = 1;
			
			cur_userchunk = entry_to_same_range_addr(cur_insert->un.ptrs.next, cur_userchunk);
		}
		
		/* we reached the end of the list */ // FIXME: use assembly-language replacement for cur_head--
	} while (!seen_object_starting_earlier
		&& find_next_nonempty_bin(&cur_head, &index_region[0], &object_minimum_size)); 
fail:
	//fprintf(stderr, "Heap index lookup failed for %p with "
	//	"cur_head %p, object_minimum_size %zu, seen_object_starting_earlier %d\n",
	//	mem, cur_head, object_minimum_size, (int) seen_object_starting_earlier);
	return NULL;
	/* FIXME: use the actual biggest allocated object, not a guess. */
}

liballocs_err_t __generic_heap_get_info(void * obj, struct uniqtype **out_type, void **out_base, 
	unsigned long *out_size, const void **out_site)
{
	++__liballocs_hit_heap_case;
	/* For heap allocations, we look up the allocation site.
	 * (This also yields an offset within a toplevel object.)
	 * Then we translate the allocation site to a uniqtypes rec location.
	 * (For direct calls in eagerly-loaded code, we can cache this information
	 * within uniqtypes itself. How? Make uniqtypes include a hash table with
	 * initial contents mapping allocsites to uniqtype recs. This hash table
	 * is initialized during load, but can be extended as new allocsites
	 * are discovered, e.g. indirect ones.)
	 */
	struct suballocated_chunk_rec *containing_suballoc = NULL;
	struct insert *heap_info = NULL;
	size_t alloc_chunksize;
	heap_info = lookup_object_info(obj, out_base, &alloc_chunksize, &containing_suballoc);
	if (!heap_info)
	{
		++__liballocs_aborted_unindexed_heap;
		return &__liballocs_err_unindexed_heap_object;
	}
	void *alloc_site_addr = (void *) ((uintptr_t) heap_info->alloc_site);

	/* Now we have a uniqtype or an allocsite. For long-lived objects 
	 * the uniqtype will have been installed in the heap header already.
	 * This is the expected case.
	 */
do_alloca_as_if_heap:
	;
	struct uniqtype *alloc_uniqtype;
	if (__builtin_expect(heap_info->alloc_site_flag, 1))
	{
		if (out_site) *out_site = NULL;
		/* Clear the low-order bit, which is available as an extra flag 
		 * bit. libcrunch uses this to track whether an object is "loose"
		 * or not. Loose objects have approximate type info that might be 
		 * "refined" later, typically e.g. from __PTR_void to __PTR_T. */
		alloc_uniqtype = (struct uniqtype *)((uintptr_t)(heap_info->alloc_site) & ~0x1ul);
	}
	else
	{
		/* Look up the allocsite's uniqtype, and install it in the heap info 
		 * (on NDEBUG builds only, because it reduces debuggability a bit). */
		uintptr_t alloc_site_addr = heap_info->alloc_site;
		void *alloc_site = (void*) alloc_site_addr;
		if (out_site) *out_site = alloc_site;
		alloc_uniqtype = allocsite_to_uniqtype(alloc_site/*, heap_info*/);
		/* Remember the unrecog'd alloc sites we see. */
		if (!alloc_uniqtype && alloc_site && 
				!__liballocs_addrlist_contains(&__liballocs_unrecognised_heap_alloc_sites, alloc_site))
		{
			__liballocs_addrlist_add(&__liballocs_unrecognised_heap_alloc_sites, alloc_site);
		}
#ifdef NDEBUG
		// install it for future lookups
		// FIXME: make this atomic using a union
		// Is this in a loose state? NO. We always make it strict.
		// The client might override us by noticing that we return
		// it a dynamically-sized alloc with a uniqtype.
		// This means we're the first query to rewrite the alloc site,
		// and is the client's queue to go poking in the insert.
		heap_info->alloc_site_flag = 1;
		heap_info->alloc_site = (uintptr_t) alloc_uniqtype /* | 0x0ul */;
#endif
	}

	if (out_size) *out_size = alloc_chunksize - sizeof (struct insert);

	// if we didn't get an alloc uniqtype, we abort
	if (!alloc_uniqtype) 
	{
		//if (__builtin_expect(k == HEAP, 1))
		//{
			++__liballocs_aborted_unrecognised_allocsite;
		//}
		//else ++__liballocs_aborted_stack;
		return &__liballocs_err_unrecognised_alloc_site;;
	}
	// else output it
	if (out_type) *out_type = alloc_uniqtype;
	
	// success
	return NULL;
}

struct allocator __generic_malloc_allocator = {
	.name = "generic malloc",
	.get_info = __generic_heap_get_info,
	.is_cacheable = 1
};
