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
    BuddyChunk* buddy_chunk = BuddyChunk::GetChunk(buddy_base, kBuddyHeapSize, false, MAX_ORDER, 0);
    ptr_to_budchunk.emplace(buddy_base, buddy_chunk);

    void* tree_base = PtrSeek(buddy_base, kBuddyHeapSize);
    int tree_size = INIT_HEAP_PAGE_SIZE - kBuddyHeapSize;

    free_tree[tree_size].insert(tree_base);

    BSTreeChunk* tree_chunk = BSTreeChunk::GetChunk(tree_base, tree_size, false, nullptr, nullptr);
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
        ptr_to_budchunk[mem_ptr]->Assign(mem_ptr, fit_size, true, fit_order, page_index);
        // if allocated size greater than request size, split free block
        if (order > fit_order) {
            size_t size = buddy_bin_size_table[order - 1];
            page_index += size;
            while (order > fit_order) {
                order--;
                void* ptr = PtrSeek(buddy_base, page_index);
                BuddyChunk* chunk = BuddyChunk::GetChunk(ptr, size, false, order, page_index);
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
        int grow_pages = max(HEAP_GROW_BY_SIZE, num_pages);
        void* ptr = SYS_MMAP_ALLOC(PageSizeToBytes(grow_pages));
        FATALIF(!SYS_MMAP_CHECK(ptr),
                "Run out of memory in allocator. Request: %d MB", grow_pages / 2);
        BSTreeChunk::GetChunk(ptr, grow_pages, false, nullptr, nullptr);
        return BSTreeAlloc(num_pages, node);
    } else {
        int size = it->first;
        unordered_set<void*>& ptrs = it->second;
        void* fit_ptr = *ptrs.begin();
        EraseTreePtr(size, fit_ptr);
        BSTreeChunk* &alloc_chunk = ptr_to_bstchunk[fit_ptr];
        if (size > num_pages) {
            // if the selected free block is larger than the request size
            BSTreeChunk* remain_chunk = alloc_chunk->Split(num_pages);
            free_tree[remain_chunk->size].insert(remain_chunk->mem_ptr);
            ptr_to_bstchunk.emplace(remain_chunk->mem_ptr, remain_chunk);
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
        BuddyChunk::PutChunk(ptr_to_budchunk[buddy_ptr]);
        ptr_to_budchunk.erase(buddy_ptr);
        free_area[order].remove(buddy_index);
        // get the beginning index of the coalesced chunk
        page_index &= buddy_index;
        order++;
    }
    ptr_to_budchunk.erase(ptr);
    free_area[order].push_front(page_index);
    void* beg_ptr = PtrSeek(buddy_base, page_index);
    cur_chunk->Assign(beg_ptr, buddy_bin_size_table[order], false, order, page_index);
    ptr_to_budchunk.emplace(beg_ptr, cur_chunk);
}

void BuddyMemoryAllocator::UpdateFreeInfo(pair<BSTreeChunk*, bool> &&p) {
    if (p.second) {
        EraseTreePtr(p.first->size, p.first->mem_ptr);
        ptr_to_bstchunk.erase(p.first->mem_ptr);
    }
}

void BuddyMemoryAllocator::BSTreeFree(void* ptr) {
    BSTreeChunk* cur_chunk = ptr_to_bstchunk[ptr];
    ptr_to_bstchunk.erase(ptr);
    cur_chunk->used = false;
    UpdateStatus(-cur_chunk->size);
    // coalesce with next chunk if it is free
    UpdateFreeInfo(cur_chunk->CoalesceNext());
    // coalesce with previous chunk if it is free
    UpdateFreeInfo(cur_chunk->CoalescePrev());
    free_tree[cur_chunk->size].insert(cur_chunk->mem_ptr);
    ptr_to_bstchunk[cur_chunk->mem_ptr] = cur_chunk;
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
