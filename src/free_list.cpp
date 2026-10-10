// Free list for mymalloc (Phase 6). See docs/phase6_free_list.md.
//
// Kept in strictly ascending address order: insertion walks once (membership
// + position), removal walks once, first-fit scans until the first fit.
// Address order makes "first fit" == "lowest-address fit" — deterministic,
// which the strategy comparison of Phase 9 can build on.
//
// Trust model (same as my_block_valid): members are validated up front by
// the callers that put them there (insert checks my_block_valid; my_free
// checks it before insert), and my_free_list_valid() re-audits everything.
// During walks, links of current members are trusted to be readable — a
// full adversarial-pointer audit remains Phase 14.
#include <mymalloc/free_list.h>

#include <mymalloc/block.h>

#include <cstdint>

namespace {

my_block_header* g_head = nullptr;
std::size_t g_count = 0;

// Links of a block assumed to be a list member (payload >= 2 pointers).
my_free_links* links_of(my_block_header* block) {
    return reinterpret_cast<my_free_links*>(my_block_to_user(block));
}

// Block -> next member via its links (NULL-safe).
my_block_header* next_member(my_block_header* block) {
    my_free_links* const links = links_of(block);
    return links->next != NULL ? my_user_to_block(links->next) : NULL;
}

// Payload big enough to hold the link pair?
int links_fit(const my_block_header* block) {
    return my_block_payload_size(block) >= sizeof(my_free_links) ? 1 : 0;
}

} // namespace

my_free_links* my_free_list_links(my_block_header* block) {
    if (block == NULL) {
        return NULL;
    }
    return reinterpret_cast<my_free_links*>(my_block_to_user(block));
}

int my_free_list_insert(my_block_header* block) {
    if (block == NULL || my_block_valid(block) != 1 || my_block_is_free(block) != 1) {
        return 0;
    }
    if (links_fit(block) != 1) {
        return 0;
    }

    const std::uintptr_t address = reinterpret_cast<std::uintptr_t>(block);

    // One walk: refuse duplicates and locate the ascending position.
    my_block_header* previous = NULL; // last member visited (all below block)
    my_block_header* current = g_head;
    while (current != NULL) {
        if (current == block) {
            return 0; // already listed: no double insert, no cycles
        }
        if (reinterpret_cast<std::uintptr_t>(current) > address) {
            break; // first member above `block` — insert before it
        }
        previous = current;
        current = next_member(current);
    }

    // Overwrite (never read) the payload links — they may hold user data.
    my_free_links* const links = links_of(block);
    links->prev = previous != NULL ? links_of(previous) : NULL;
    links->next = current != NULL ? links_of(current) : NULL;
    if (previous != NULL) {
        links_of(previous)->next = links;
    } else {
        g_head = block; // lowest address so far becomes the head
    }
    if (current != NULL) {
        links_of(current)->prev = links;
    }
    ++g_count;
    return 1;
}

int my_free_list_remove(my_block_header* block) {
    if (block == NULL) {
        return 0;
    }
    my_block_header* previous = NULL;
    my_block_header* current = g_head;
    while (current != NULL && current != block) {
        previous = current;
        current = next_member(current);
    }
    if (current == NULL) {
        return 0; // not a member
    }

    my_free_links* const links = links_of(block);
    my_free_links* const after = links->next;
    if (previous != NULL) {
        links_of(previous)->next = after;
    } else {
        g_head = after != NULL ? my_user_to_block(after) : NULL;
    }
    if (after != NULL) {
        after->prev = previous != NULL ? links_of(previous) : NULL;
    }
    links->prev = NULL; // clear stale links so the freed payload is not
    links->next = NULL; // mistaken for membership later
    --g_count;
    return 1;
}

my_block_header* my_free_list_first_fit(size_t min_block_size) {
    if (min_block_size == 0) {
        return NULL; // defensive: a real request always includes the header
    }
    for (my_block_header* current = g_head; current != NULL;) {
        if (current->size >= min_block_size) {
            return current; // lowest-address fit (address-ordered list)
        }
        current = next_member(current);
    }
    return NULL;
}

my_block_header* my_free_list_head(void) {
    return g_head;
}

size_t my_free_list_count(void) {
    return g_count;
}

int my_free_list_valid(void) {
    std::size_t walked = 0;
    my_block_header* previous = NULL;
    std::uintptr_t previous_address = 0;
    my_block_header* current = g_head;
    while (current != NULL) {
        if (walked >= g_count) {
            return 0; // more entries than accounted for (or a cycle)
        }
        if (my_block_valid(current) != 1) {
            return 0;
        }
        if (my_block_is_free(current) != 1) {
            return 0; // listed blocks must be free
        }
        if (links_fit(current) != 1) {
            return 0; // too small to hold its own links
        }
        const std::uintptr_t address = reinterpret_cast<std::uintptr_t>(current);
        if (previous != NULL && address <= previous_address) {
            return 0; // strictly ascending: wrong order or a cycle
        }
        my_free_links* const links = links_of(current);
        const my_free_links* expected_prev = previous != NULL ? links_of(previous) : NULL;
        if (links->prev != expected_prev) {
            return 0; // reciprocity broken (first entry must have NULL prev)
        }
        ++walked;
        previous = current;
        previous_address = address;
        current = links->next != NULL ? my_user_to_block(links->next) : NULL;
    }
    return walked == g_count ? 1 : 0; // count must match exactly
}

void my_free_list_clear(void) {
    g_head = NULL;
    g_count = 0;
}