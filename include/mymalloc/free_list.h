// mymalloc — free list (Phase 6).
//
// Structure: an intrusive, address-ascending doubly linked list whose links
// live in the PAYLOAD of every free block (the plan documented in
// docs/phase4_block_metadata.md §8; design in docs/phase6_free_list.md):
//
//   block start                     user pointer (= links overlay)
//   v                               v
//   [ size | free | prev | next ][ fl_prev | fl_next | ...unused... ]
//   <---------- size ------------->
//
// The header's prev/next stay purely PHYSICAL neighbors (Phase 4); logical
// free-list membership is stored only while a block is free, so allocated
// blocks pay zero extra bytes and the header remains 32 bytes on x86-64.
//
// Invariants (machine-checked by my_free_list_valid):
//   1. every listed block passes my_block_valid and has free == 1,
//   2. its payload can hold my_free_links (>= sizeof(my_free_links)),
//   3. entries appear in STRICTLY ascending address order (which also rules
//      out cycles),
//   4. fl_prev/fl_next are reciprocal with the walk order and NULL at the
//      ends,
//   5. the walk visits exactly my_free_list_count() entries.
//
// Not thread-safe (same policy as every phase so far).
#ifndef MYMALLOC_FREE_LIST_H
#define MYMALLOC_FREE_LIST_H

#include <stddef.h>

#include <mymalloc/block.h>

// Links overlaid on a free block's payload (at my_block_to_user(block)).
typedef struct my_free_links {
    struct my_free_links* prev; // previous entry in address order, or NULL
    struct my_free_links* next; // next entry in address order, or NULL
} my_free_links;

// Payload -> links (NULL-safe). The payload is only meaningful while the
// block is in the list; insert() overwrites both links WITHOUT reading them,
// so stale user data can never be mistaken for list state.
my_free_links* my_free_list_links(my_block_header* block);

// Inserts `block` in ascending address order (O(n): one walk proves
// non-membership and finds the position). Returns 1 on success, 0 when the
// block is NULL, invalid, not free, too small to hold the links, or ALREADY
// in the list (defensive double-insert guard).
int my_free_list_insert(my_block_header* block);

// Removes `block` from the list and clears its links. Returns 1 when it was
// a member, 0 when it was not (including NULL). Does not change free state.
int my_free_list_remove(my_block_header* block);

// First fit: the LOWEST-address free block whose total size (header included,
// matching my_block_header::size) is >= min_block_size, else NULL.
// min_block_size == 0 returns NULL (defensive). Does not modify the list.
my_block_header* my_free_list_first_fit(size_t min_block_size);

// Head of the list (lowest address) or NULL on empty.
my_block_header* my_free_list_head(void);
// Number of listed blocks (exact; maintained by insert/remove/clear).
size_t my_free_list_count(void);

// Walks the whole list verifying every invariant above. Returns 1 when the
// list is consistent, 0 otherwise. Ordered defensively like my_block_valid:
// address-level facts are established before any link is followed, and the
// walk is bounded by the count, so corruption cannot loop it forever.
int my_free_list_valid(void);

// Drops the list WITHOUT touching the blocks — teardown for wholesale region
// release (Phase 12) and test cleanup. Blocks remain free but unlisted.
void my_free_list_clear(void);

#endif // MYMALLOC_FREE_LIST_H