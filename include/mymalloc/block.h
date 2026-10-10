// mymalloc — block metadata (Phase 4).
//
// Every block (allocated or free) starts with a header immediately followed
// by its payload:
//
//   block start                 user pointer
//   v                           v
//   [ size | free | prev | next ][ payload ... ]  ... [ next block header ]
//   <--------- size ----------->
//
// - `size` is the TOTAL block size in bytes (header + payload), always a
//   multiple of my_default_alignment() and at least my_block_user_offset().
// - `prev`/`next` point to the physically adjacent blocks in address order
//   (NULL at region edges) — this gives O(1) neighbor discovery for
//   coalescing (see docs/phase4_block_metadata.md for the tradeoff).
// - `free` is exactly 0 (allocated) or 1 (free).
//
// my_block_valid() verifies all invariants; it is ordered so that
// misaligned or structurally impossible pointers are rejected WITHOUT
// being dereferenced.
#ifndef MYMALLOC_BLOCK_H
#define MYMALLOC_BLOCK_H

#include <stddef.h>

typedef struct my_block_header my_block_header;

struct my_block_header {
    size_t size;           // total bytes incl. header; multiple of alignment
    my_block_header* prev; // adjacent block at a lower address, or NULL
    my_block_header* next; // adjacent block at a higher address, or NULL
    int free;              // 1 = free, 0 = allocated
};

// Bytes from a block start to its user pointer:
// align_up(sizeof(my_block_header), my_default_alignment()).
size_t my_block_user_offset(void);

// Header -> user pointer (NULL in -> NULL out).
void* my_block_to_user(my_block_header* block);

// User pointer -> header; exact inverse of my_block_to_user
// (NULL in -> NULL out).
my_block_header* my_user_to_block(void* user);

// Payload bytes available at the user pointer: size - user_offset.
// Precondition: `block` points to a trusted/readable header.
size_t my_block_payload_size(const my_block_header* block);

// Initializes the header at `base` with total `size`, state `is_free`
// (0 or 1) and the given neighbors. Returns 1 on success, 0 when:
// - base is NULL or not aligned to my_default_alignment(),
// - size < my_block_user_offset() or size is not an alignment multiple,
// - is_free is not 0 or 1.
int my_block_init(my_block_header* base, size_t size, int is_free, my_block_header* prev,
                  my_block_header* next);

// Splits `block` into two physically adjacent blocks: a FRONT of `front_size`
// bytes that keeps `block`'s address, `prev`, and free state, and a REMAINDER
// occupying the rest of the region at (block + front_size). Repairs the
// physical neighbor chain (front->next = remainder, remainder->prev = front,
// the old upper neighbor is re-linked to the remainder). Returns 1 on success,
// 0 when:
// - block is NULL or fails my_block_valid,
// - front_size is 0, not a multiple of my_default_alignment(), or
//   < my_block_user_offset(),
// - front_size >= block->size (no remainder would be left), or
// - the remainder (block->size - front_size) < my_block_user_offset()
//   (too small to be a legal block).
//
// Both halves inherit `block`'s free state; the caller adjusts each as needed.
// This touches only headers and physical links — free-list membership is the
// caller's responsibility (see docs/phase7_splitting.md).
int my_block_split(my_block_header* block, size_t front_size);

// Neighbor accessors (NULL-safe). Precondition: trusted/readable header.
my_block_header* my_block_prev(const my_block_header* block);
my_block_header* my_block_next(const my_block_header* block);

// True when the block's state is free. Precondition: trusted header.
int my_block_is_free(const my_block_header* block);

// Verifies every metadata invariant documented in
// docs/phase4_block_metadata.md. Misaligned or NULL pointers are rejected
// without dereferencing. Link pointers are trusted to point to readable
// memory (full adversarial-pointer validation is Phase 14).
int my_block_valid(const my_block_header* block);

#endif // MYMALLOC_BLOCK_H