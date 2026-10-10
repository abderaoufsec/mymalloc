// Block metadata for mymalloc (Phase 4). See docs/phase4_block_metadata.md.
//
// Validation is ordered defensively: address-level facts (NULL, alignment,
// size sanity, neighbor ADDRESSES) are checked before anything dereferences
// a link pointer, so structural corruption cannot make the validator read
// through a bogus pointer that fails the address checks.
#include <mymalloc/block.h>

#include <mymalloc/alignment.h>

#include <cstdint>

size_t my_block_user_offset(void) {
    // Magic static: offset that keeps the user pointer on the default
    // alignment, given block starts are alignment multiples themselves.
    static const std::size_t offset = [] {
        std::size_t aligned = 0;
        if (my_align_up_size(sizeof(my_block_header), my_default_alignment(), &aligned) == 1) {
            return aligned;
        }
        return sizeof(my_block_header); // unreachable: sizeof cannot overflow here
    }();
    return offset;
}

void* my_block_to_user(my_block_header* block) {
    if (block == NULL) {
        return NULL;
    }
    return reinterpret_cast<unsigned char*>(block) + my_block_user_offset();
}

my_block_header* my_user_to_block(void* user) {
    if (user == NULL) {
        return NULL;
    }
    return reinterpret_cast<my_block_header*>(reinterpret_cast<unsigned char*>(user) -
                                              my_block_user_offset());
}

size_t my_block_payload_size(const my_block_header* block) {
    if (block == NULL || block->size < my_block_user_offset()) {
        return 0;
    }
    return block->size - my_block_user_offset();
}

int my_block_init(my_block_header* base, size_t size, int is_free, my_block_header* prev,
                  my_block_header* next) {
    if (base == NULL) {
        return 0;
    }
    if ((reinterpret_cast<uintptr_t>(base) % my_default_alignment()) != 0) {
        return 0;
    }
    const std::size_t offset = my_block_user_offset();
    if (size < offset || (size % my_default_alignment()) != 0) {
        return 0;
    }
    if (is_free != 0 && is_free != 1) {
        return 0;
    }
    base->size = size;
    base->free = is_free;
    base->prev = prev;
    base->next = next;
    return 1;
}

int my_block_split(my_block_header* block, size_t front_size) {
    // The block must be a well-formed, valid header before we carve it.
    if (my_block_valid(block) != 1) {
        return 0;
    }
    const std::size_t alignment = my_default_alignment();
    const std::size_t offset = my_block_user_offset();
    // The front must itself be a legal block and must leave a remainder.
    if (front_size == 0 || (front_size % alignment) != 0 || front_size < offset) {
        return 0;
    }
    if (front_size >= block->size) {
        return 0; // no remainder would be left
    }
    const std::size_t remainder_size = block->size - front_size;
    if (remainder_size < offset) {
        return 0; // the remainder could not be a valid block
    }

    my_block_header* const old_next = block->next;
    my_block_header* const remainder =
        reinterpret_cast<my_block_header*>(reinterpret_cast<unsigned char*>(block) + front_size);

    // FRONT: same address, same prev, same free state; only size/next change.
    block->size = front_size;
    block->next = remainder;

    // REMAINDER: fills the tail, inherits the free state (block->free is left
    // untouched above), and is wired into the physical chain between the front
    // and the old upper neighbor.
    remainder->size = remainder_size;
    remainder->free = block->free;
    remainder->prev = block;
    remainder->next = old_next;
    if (old_next != NULL) {
        old_next->prev = remainder;
    }
    return 1;
}

my_block_header* my_block_prev(const my_block_header* block) {
    return block == NULL ? NULL : block->prev;
}

my_block_header* my_block_next(const my_block_header* block) {
    return block == NULL ? NULL : block->next;
}

int my_block_is_free(const my_block_header* block) {
    return block == NULL ? 0 : block->free;
}

int my_block_valid(const my_block_header* block) {
    if (block == NULL) {
        return 0;
    }
    const uintptr_t address = reinterpret_cast<uintptr_t>(block);
    if ((address % my_default_alignment()) != 0) {
        return 0; // misaligned: reject before any dereference
    }
    const std::size_t offset = my_block_user_offset();
    if (block->size < offset || (block->size % my_default_alignment()) != 0) {
        return 0;
    }
    if (block->free != 0 && block->free != 1) {
        return 0;
    }
    if (block->size > UINTPTR_MAX - address) {
        return 0; // corrupted size: the end address would wrap
    }
    const uintptr_t block_end = address + block->size;

    // next: address check first (no dereference of a wrong pointer).
    if (block->next != NULL) {
        if (reinterpret_cast<uintptr_t>(block->next) != block_end) {
            return 0;
        }
        if (block->next->prev != block) {
            return 0; // reciprocity broken
        }
    }
    // prev: must sit at a lower address, be adjacent, and link back.
    if (block->prev != NULL) {
        const uintptr_t prev_address = reinterpret_cast<uintptr_t>(block->prev);
        if (prev_address >= address) {
            return 0; // reject before dereferencing
        }
        if (block->prev->next != block) {
            return 0; // reciprocity broken
        }
        if (block->prev->size > UINTPTR_MAX - prev_address ||
            prev_address + block->prev->size != address) {
            return 0; // prev must end exactly where this block starts
        }
    }
    return 1;
}