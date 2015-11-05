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
      kBuddyPageSize(1 << MAX_ORDER) {
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

int BuddyMemoryAllocator::BuddyBlockSize(int order) {
    return 1 << order;
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
    buddy_base = reinterpret_cast<char*>(new_chunk);
    free_area[MAX_ORDER].emplace_back(0);
    BuddyChunk* buddy_chunk = new BuddyChunk(buddy_base, kBuddyPageSize, false, MAX_ORDER, 0);
    ptr_to_budchunk.emplace(buddy_base, buddy_chunk);

    void* tree_base = reinterpret_cast<void*>(buddy_base + PageSizeToBytes(kBuddyPageSize));
    int tree_size = INIT_HEAP_PAGE_SIZE - kBuddyPageSize;

    free_tree.emplace(tree_size, tree_base);

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
    if (num_pages <= kBuddyPageSize) {
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
        void* res_ptr = SYS_MMAP_ALLOC(kHashSegAlignedSize);
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
    UpdateStatus(kHashSegPageSize);
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
        void* mem_ptr = reinterpret_cast<void*>(buddy_base + PageSizeToBytes(page_index));
        BuddyChunk* fit_chunk = ptr_to_budchunk[mem_ptr];
        fit_chunk->set(mem_ptr, fit_size, true, fit_order, page_index);
        ptr_to_budchunk.emplace(fit_chunk->mem_ptr, fit_chunk);
        // if allocated size greater than request size, split free block
        if (order > fit_order) {
            size_t size = buddy_bin_size_table[order - 1];
            page_index += size;
            while (order > fit_order) {
                order--;
                void* ptr = reinterpret_cast<void*>(buddy_base + PageSizeToBytes(page_index));
                // cout << "size:" << size << " index:" << page_index << " order:" << order
                //      << " ptr:" << ((long)ptr) / (512*1024) << endl;
                BuddyChunk* chunk = new BuddyChunk(ptr, size, false, order, page_index);
                ptr_to_budchunk.emplace(ptr, chunk);
                free_area[order].emplace_back(page_index);
                size >>= 1;
                page_index -= size;
            }
        }
        // cout << "ptr:" << ((long)fit_chunk->mem_ptr) / (512*1024) << endl;
        return fit_chunk->mem_ptr;
    }
    return nullptr;
}

 void* BuddyMemoryAllocator::BSTreeAlloc(int num_pages, int node) {
    auto it = free_tree.lower_bound(num_pages);
    if (it == free_tree.end()) {
        return nullptr;
    } else {
        BSTreeChunk* alloc_chunk = ptr_to_bstchunk[it->second];
        // if exactly same size free block is found
        if (it->first == num_pages) {
            alloc_chunk->used = true;
            free_tree.erase(it);
        } else {
            // if the selected free block is larger than the request size
            // get the pointer to the beginning of the remain free size and insert into free tree
            void* remain = reinterpret_cast<void*>(reinterpret_cast<char*>(alloc_chunk->mem_ptr) +
                                                   PageSizeToBytes(num_pages));
            int remain_size = it->first - num_pages;
            free_tree.emplace(remain_size, remain);

            BSTreeChunk* remain_chunk = new BSTreeChunk(remain, remain_size, false);
            alloc_chunk->next = remain_chunk;
            remain_chunk->prev = alloc_chunk;
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
    if (!ptr) {
        return;
    }

    lock_guard<mutex> lck(mtx_);
    if (occupied_hash_segs.find(ptr) != occupied_hash_segs.end()) {
        reserved_hash_segs.push_back(ptr);
        occupied_hash_segs.erase(ptr);
        UpdateStatus(-kHashSegPageSize);
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
    void* beg_ptr = cur_chunk->mem_ptr;
    int beg_index = page_index;
    for (; order <= MAX_ORDER; order++) {
        int buddy_index = page_index ^ (1 << order);
        void* buddy_ptr = reinterpret_cast<void*>(buddy_base + PageSizeToBytes(buddy_index));
        if (ptr_to_budchunk.find(buddy_ptr) == ptr_to_budchunk.end() ||
            ptr_to_budchunk[buddy_ptr]->used != false ||
            ptr_to_budchunk[buddy_ptr]->order != order) {
            break;
        }
        ptr_to_budchunk.erase(buddy_ptr);
        beg_ptr = min(beg_ptr, buddy_ptr);
        beg_index = min(beg_index, buddy_index);
        page_index &= buddy_index;
    }
    cur_chunk->set(beg_ptr, buddy_bin_size_table[order], false, order, beg_index);
    ptr_to_budchunk.emplace(beg_ptr, cur_chunk);
}

void BuddyMemoryAllocator::BSTreeFree(void* ptr) {
    BSTreeChunk* cur_chunk = ptr_to_bstchunk[ptr];
    cur_chunk->used = false;
    // point to the beginning of the contiguous memory
    void* beg_ptr = cur_chunk->mem_ptr;
    // total size of physical contiguous memory
    int size = cur_chunk->size;
    UpdateStatus(-size);
    // iterative coalesce with previous chunk if it is free
    BSTreeChunk* prevChunk = cur_chunk->prev;
    while (prevChunk) {
        if (true == prevChunk->used)
            break;
        size += prevChunk->size;
        // update beginning pointer
        beg_ptr = prevChunk->mem_ptr;
        prevChunk = prevChunk->prev;
    }
    // iterative coalesce with next chunk if it is free
    BSTreeChunk* nextChunk = cur_chunk->next;
    while (nextChunk) {
        if (true == nextChunk->used)
            break;
        size += nextChunk->size;
        nextChunk = nextChunk->next;
    }

    free_tree.emplace(size, beg_ptr);
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
