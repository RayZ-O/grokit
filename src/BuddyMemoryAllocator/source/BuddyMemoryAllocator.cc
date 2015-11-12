//
//  Copyright 2015 Rui Zhang, 2012 Alin Dobra and Christopher Jermaine
//
//  Licensed under the Apache License, Version 2.0 (the "License");
//  you may not use this file except in compliance with the License.
//  You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
//  Unless required by applicable law or agreed to in writing, software
//  distributed under the License is distributed on an "AS IS" BASIS,
//  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
//  See the License for the specific language governing permissions and
//  limitations under the License.
//
#include <sys/mman.h>
#include <iostream>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <string.h>

#include "MmapAllocator.h"
#include "Errors.h"
#include "Numa.h"
#include "BuddyMemoryAllocator.h"

using namespace std;

void* mmap_alloc_imp(size_t noBytes, int node, const char* f, int l) {
    BuddyMemoryAllocator& aloc = BuddyMemoryAllocator::GetAllocator();
    void* rez = aloc.MmapAlloc(noBytes, node, f, l);
    return rez;
}

void mmap_prot_read_imp(void* ptr, const char* f, int l) {
    BuddyMemoryAllocator& aloc = BuddyMemoryAllocator::GetAllocator();
    aloc.MmapChangeProt(ptr, PROT_READ);
}

void mmap_prot_readwrite_imp(void* ptr, const char* f, int l) {
    BuddyMemoryAllocator& aloc = BuddyMemoryAllocator::GetAllocator();
    aloc.MmapChangeProt(ptr, PROT_READ | PROT_WRITE);
}

void mmap_free_imp(void* ptr, const char* f, int l) {
    if(!ptr) {
        WARNING("Warning: Attempted free of null pointer at %s:%d", f, l);
    }

    BuddyMemoryAllocator& aloc = BuddyMemoryAllocator::GetAllocator();
    aloc.MmapFree(ptr);
}

off_t mmap_used(void) {
    BuddyMemoryAllocator& aloc = BuddyMemoryAllocator::GetAllocator();
    return PAGES_TO_BYTES(aloc.AllocatedPages());
}

BuddyMemoryAllocator::BuddyMemoryAllocator(void)
    : is_initialized_(false),  // google code stype constructor initializer lists
      allocated_pages_(0),
      free_pages_(0),
      kHashSegPageSize(BytesToPageSize(HASH_SEG_SIZE)),
      kHashSegAlignedSize(PageSizeToBytes(kHashSegPageSize)),
      kBuddyHeapSize(1 << MAX_ORDER) {
    for (int order = 0; order <= MAX_ORDER; order++) {
        buddy_bin_size_table.push_back(1 << order);
    }
    free_area.resize(MAX_ORDER + 1);
}

BuddyMemoryAllocator::~BuddyMemoryAllocator(void) {

}

int BuddyMemoryAllocator::BytesToPageSize(size_t bytes) {
    // compute the size in pages
    int page_size = bytes >> ALLOC_PAGE_SIZE_EXPONENT;
    if (bytes != PageSizeToBytes(page_size))
        page_size++; // extra page to get the overflow

    return page_size;
}

size_t BuddyMemoryAllocator::PageSizeToBytes(int page_size) {
    return static_cast<size_t>(page_size) << ALLOC_PAGE_SIZE_EXPONENT;
}

void* BuddyMemoryAllocator::PtrSeek(void* ptr, int num_pages) {
    char* res = reinterpret_cast<char*>(ptr) + PageSizeToBytes(num_pages);
    return reinterpret_cast<void*>(res);
}

void BuddyMemoryAllocator::HeapInit() {
    is_initialized_ = true;

    void *new_chunk = SYS_MMAP_ALLOC(PageSizeToBytes(INIT_HEAP_PAGE_SIZE));
    if (!SYS_MMAP_CHECK(new_chunk)) {
        perror("BuddyMemoryAllocator");
        FATAL("The memory allocator could not allocate memory");
    }
    // TODO for numa-aware allocator, init heap page size will be
    // number of nodes * INIT_HEAP_PAGE_SIZE
    free_pages_ = INIT_HEAP_PAGE_SIZE;
    // initalized buddy system
    buddy_base = new_chunk;
    free_area[MAX_ORDER].emplace_back(0);
    BuddyChunk* buddy_chunk = GetBuddyChunk(buddy_base, kBuddyHeapSize, false, MAX_ORDER, 0);
    ptr_to_budchunk.emplace(buddy_base, buddy_chunk);

    void* tree_base = PtrSeek(buddy_base, kBuddyHeapSize);
    int tree_size = INIT_HEAP_PAGE_SIZE - kBuddyHeapSize;

    free_tree[tree_size].insert(tree_base);

    BSTreeChunk* tree_chunk = new BSTreeChunk(tree_base, tree_size, false);
    ptr_to_bstchunk.emplace(tree_base, tree_chunk);
}

void* BuddyMemoryAllocator::MmapAlloc(size_t num_bytes, int node, const char* f, int l) {
    if (0 == num_bytes)
        return nullptr;

    lock_guard<mutex> lck(mtx_);

    if (!is_initialized_)
        HeapInit();

    int num_pages = BytesToPageSize(num_bytes);
    // TODO make hash seg page size a constexpr to avoid evaluate every time
    void* res_ptr = nullptr;
    if (kHashSegPageSize == num_pages) {
        return HashSegAlloc();
    }
    if (num_pages <= kBuddyHeapSize) {
        res_ptr = BuddyAlloc(num_pages, node);
    }
    if (!res_ptr) {
        res_ptr = BSTreeAlloc(num_pages, node);
    }
    return res_ptr;
}

void* BuddyMemoryAllocator::HashSegAlloc() {
    void* res_ptr = nullptr;
    if (reserved_hash_segs.empty()) {
        // may use page aligned size
        res_ptr = SYS_MMAP_ALLOC(kHashSegAlignedSize);
        if (!SYS_MMAP_CHECK(res_ptr)){
            perror("BuddyMemoryAllocator");
            FATAL("The memory allocator could not allocate memory");
        }
    } else {
        res_ptr = reserved_hash_segs.back();
        reserved_hash_segs.pop_back();
    }
    occupied_hash_segs.emplace(res_ptr);
    SYS_MMAP_PROT(res_ptr, kHashSegAlignedSize, PROT_READ | PROT_WRITE);
    // UpdateStatus(kHashSegPageSize);
    return res_ptr;
}

int BuddyMemoryAllocator::GetOrder(int page_size) {
    int order = 0;
    int n = 1;
    while(n < page_size) {
        n <<= 1;
        order++;
    }
    return order;
}

BuddyMemoryAllocator::BuddyChunk*
BuddyMemoryAllocator::GetBuddyChunk(void* ptr, int size, bool used, int order, int idx) {
    BuddyChunk* chunk = nullptr;
    if (budchunk_pool.empty())
        chunk = new BuddyChunk(ptr, size, used, order, idx);
    else {
        chunk = budchunk_pool.back();
        budchunk_pool.pop_back();
        chunk->set(ptr, size, used, order, idx);
    }
    return chunk;
}

void* BuddyMemoryAllocator::BuddyAlloc(int num_pages, int node) {
    int fit_order = GetOrder(num_pages);
    for (int order = fit_order; order <= MAX_ORDER; order++) {
        if (free_area[order].empty()) {
            continue;
        }
        size_t fit_size = buddy_bin_size_table[fit_order];
        UpdateStatus(fit_size);
        // get the page index of the first page in the free block
        int page_index = free_area[order].front();
        free_area[order].pop_front();
        // get the pointer pointing to the start of the free block
        void* mem_ptr = PtrSeek(buddy_base, page_index);
        ptr_to_budchunk[mem_ptr]->set(mem_ptr, fit_size, true, fit_order, page_index);
        // if allocated size greater than request size, split free block
        if (order > fit_order) {
            size_t size = buddy_bin_size_table[order - 1];
            page_index += size;
            while (order > fit_order) {
                order--;
                void* ptr = PtrSeek(buddy_base, page_index);
                BuddyChunk* chunk = GetBuddyChunk(ptr, size, false, order, page_index);
                assert(chunk != nullptr);
                ptr_to_budchunk.emplace(ptr, chunk);
                free_area[order].emplace_back(page_index);
                size >>= 1;
                page_index -= size;
            }
        }
        return mem_ptr;
    }
    return nullptr;
}

void BuddyMemoryAllocator::EraseTreePtr(int size, void* ptr) {
    auto it = free_tree.find(size);
    if (it->second.size() == 1)
        free_tree.erase(it);
    else
        it->second.erase(ptr);
}

void* BuddyMemoryAllocator::BSTreeAlloc(int num_pages, int node) {
    auto it = free_tree.lower_bound(num_pages);
    if (it == free_tree.end()) {
        return nullptr;
    } else {
        int size = it->first;
        unordered_set<void*> &ptrs = it->second;
        void* fit_ptr = *ptrs.begin();
        EraseTreePtr(size, fit_ptr);
        BSTreeChunk* &alloc_chunk = ptr_to_bstchunk[fit_ptr];
        alloc_chunk->set(fit_ptr, num_pages, true);
        if (size > num_pages) {
            // if the selected free block is larger than the request size
            // get the pointer to the beginning of the remain free size and insert into free tree
            void* remain = PtrSeek(fit_ptr, num_pages);
            int remain_size = size - num_pages;
            assert(remain != nullptr);
            free_tree[remain_size].insert(remain);
            BSTreeChunk* remain_chunk = new BSTreeChunk(remain, remain_size, false);
            // insert remain chunk into sibling list
            remain_chunk->next = alloc_chunk->next;
            remain_chunk->prev = alloc_chunk;
            alloc_chunk->next = remain_chunk;
            ptr_to_bstchunk.emplace(remain, remain_chunk);
        }
        UpdateStatus(num_pages);
        return alloc_chunk->mem_ptr;
    }
}

void BuddyMemoryAllocator::MmapChangeProt(void* ptr, int prot) {
    if (!ptr) {
        return;
    }

    lock_guard<mutex> lck(mtx_);
    if (occupied_hash_segs.find(ptr) != occupied_hash_segs.end()) {
        SYS_MMAP_PROT(ptr, kHashSegAlignedSize, prot);
    } else if (ptr_to_budchunk.find(ptr) != ptr_to_budchunk.end()) {
        WARNINGIF(-1 == SYS_MMAP_PROT(ptr, PageSizeToBytes(ptr_to_budchunk[ptr]->size), prot),
                "Changing protection of page at address %p size %d failed with message %s",
                ptr, ptr_to_budchunk[ptr]->size, strerror(errno));
    } else {
        // find the size and insert the freed memory in the
        auto it = ptr_to_bstchunk.find(ptr);
        FATALIF(it == ptr_to_bstchunk.end(), "Changing the protection of unallocated pointer %p.", ptr);
         // change protection
        WARNINGIF(-1 == SYS_MMAP_PROT(ptr, PageSizeToBytes(it->second->size), prot),
                "Changing protection of page at address %p size %d failed with message %s",
                ptr, it->second->size, strerror(errno));
    }
}

void BuddyMemoryAllocator::MmapFree(void* ptr) {
    if (!ptr)
        return;

    lock_guard<mutex> lck(mtx_);
    if (occupied_hash_segs.find(ptr) != occupied_hash_segs.end()) {
        reserved_hash_segs.push_back(ptr);
        occupied_hash_segs.erase(ptr);
        // UpdateStatus(-kHashSegPageSize);
    } else if (ptr_to_budchunk.find(ptr) != ptr_to_budchunk.end()) {
        BuddyFree(ptr);
    } else {
        FATALIF(ptr_to_bstchunk.find(ptr) == ptr_to_bstchunk.end(), "Freeing unallocated pointer %p.", ptr);
        BSTreeFree(ptr);
    }
}

void BuddyMemoryAllocator::BuddyFree(void* ptr) {
    BuddyChunk* cur_chunk = ptr_to_budchunk[ptr];
    int order = cur_chunk->order;
    UpdateStatus(-buddy_bin_size_table[order]);
    int page_index = cur_chunk->page_index;
    while (order <= MAX_ORDER) {
        int buddy_index = page_index ^ buddy_bin_size_table[order];
        void* buddy_ptr = PtrSeek(buddy_base, buddy_index);
        if (ptr_to_budchunk.find(buddy_ptr) == ptr_to_budchunk.end()) // buddy pointer not in pointer map
            break;
        if (ptr_to_budchunk[buddy_ptr]->used != false) // buddy chunk in use
            break;
        if (ptr_to_budchunk[buddy_ptr]->order != order)   // buddy chunk not in the same order
            break;
        // collect unused buddy chunk to buddy chunk pool to avoid frequently allocating and deallocating
        budchunk_pool.push_back(ptr_to_budchunk[buddy_ptr]);
        ptr_to_budchunk.erase(buddy_ptr);
        free_area[order].remove(buddy_index);
        // get the beginning index of the coalesced chunk
        page_index &= buddy_index;
        order++;
    }
    ptr_to_budchunk.erase(ptr);
    free_area[order].push_front(page_index);
    void* beg_ptr = PtrSeek(buddy_base, page_index);
    cur_chunk->set(beg_ptr, buddy_bin_size_table[order], false, order, page_index);
    ptr_to_budchunk.emplace(beg_ptr, cur_chunk);
}

void BuddyMemoryAllocator::BSTreeFree(void* ptr) {
    BSTreeChunk* cur_chunk = ptr_to_bstchunk[ptr];
    // total size of physical contiguous memory
    int size = cur_chunk->size;
    UpdateStatus(-size);
    // iterative coalesce with next chunk if it is free
    BSTreeChunk* nextChunk = cur_chunk->next;
    while (nextChunk) {
        if (nextChunk->used)
            break;
        assert(nextChunk->mem_ptr != nullptr);
        // assert(free_tree[nextChunk->size].find(nextChunk->mem_ptr) != free_tree[nextChunk->size].end());
        EraseTreePtr(nextChunk->size, nextChunk->mem_ptr);
        // assert(ptr_to_bstchunk.find(nextChunk->mem_ptr) != ptr_to_bstchunk.end());
        ptr_to_bstchunk.erase(nextChunk->mem_ptr);
        size += nextChunk->size;
        nextChunk = nextChunk->next;
    }
    // iterative coalesce with previous chunk if it is free
    BSTreeChunk* prevChunk = cur_chunk->prev;
    while (prevChunk) {
        if (prevChunk->used)
            break;
        assert(prevChunk->mem_ptr != nullptr);
        EraseTreePtr(prevChunk->size, prevChunk->mem_ptr);
        // assert(ptr_to_bstchunk.find(prevChunk->mem_ptr) != ptr_to_bstchunk.end());
        ptr_to_bstchunk.erase(prevChunk->mem_ptr);
        size += prevChunk->size;
        // the beginning of new coalesce chunk
        cur_chunk = prevChunk;
        prevChunk = prevChunk->prev;
    }
    ptr_to_bstchunk.erase(ptr);
    free_tree[size].insert(cur_chunk->mem_ptr);
    cur_chunk->set(cur_chunk->mem_ptr, size, false, prevChunk, nextChunk);
    ptr_to_bstchunk.emplace(cur_chunk->mem_ptr, cur_chunk);
}

void BuddyMemoryAllocator::UpdateStatus(int allocated_size) {
    free_pages_ -= allocated_size;
    allocated_pages_ += allocated_size;
}

size_t BuddyMemoryAllocator::AllocatedPages() const {
    return allocated_pages_;
}

size_t BuddyMemoryAllocator::FreePages() const {
    return free_pages_;
}
