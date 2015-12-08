//
//  Copyright 2012 Alin Dobra and Christopher Jermaine
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
#include "NumaMemoryAllocator.h"

using namespace std;

void* mmap_alloc_imp(size_t noBytes, int node, const char* f, int l){
	NumaMemoryAllocator& aloc=NumaMemoryAllocator::GetAllocator();
	void* rez= aloc.MmapAlloc(noBytes, node, f, l);
	return rez;
}

void mmap_prot_read_imp(void* ptr, const char* f, int l){
	NumaMemoryAllocator& aloc=NumaMemoryAllocator::GetAllocator();
	aloc.MmapChangeProt(ptr, PROT_READ);
}

void mmap_prot_readwrite_imp(void* ptr, const char* f, int l){
	NumaMemoryAllocator& aloc=NumaMemoryAllocator::GetAllocator();
	aloc.MmapChangeProt(ptr, PROT_READ | PROT_WRITE);
}

void mmap_free_imp(void* ptr, const char* f, int l){
    if( ptr == NULL ) {
        WARNING("Warning: Attempted free of null pointer at %s:%d", f, l);
    }

	NumaMemoryAllocator& aloc=NumaMemoryAllocator::GetAllocator();
	aloc.MmapFree(ptr);
}

off_t mmap_used(void){
	NumaMemoryAllocator& aloc=NumaMemoryAllocator::GetAllocator();
	return PAGES_TO_BYTES(aloc.AllocatedPages());
}


NumaMemoryAllocator::NumaMemoryAllocator(void)
    : is_initialized_(false),  // google code stype constructor initializer lists
      allocated_pages_(0),
      free_pages_(0),
      kHashSegPageSize(BytesToPageSize(HASH_SEG_SIZE)),  // hash segment size in pages
      kHashSegAlignedSize(PageSizeToBytes(kHashSegPageSize)) // hash segment size in bytes
    {}

NumaMemoryAllocator::~NumaMemoryAllocator(void) {
    // for (auto p : ptr_to_bstchunk) {
    //     SYS_MMAP_FREE(p.first, PageSizeToBytes(p.second->size));
    // }
    // for (auto s: reserved_hash_segs) {
    //     SYS_MMAP_FREE(s, kHashSegAlignedSize);
    // }
    // NumaMemoryAllocator::ChunkInfo::FreeChunks();
}

size_t NumaMemoryAllocator::PageSizeToBytes(int page_size) {
    return static_cast<size_t>(page_size) << ALLOC_PAGE_SIZE_EXPONENT;
}

int NumaMemoryAllocator::BytesToPageSize(size_t bytes) {
    // compute the size in pages
    int page_size = bytes >> ALLOC_PAGE_SIZE_EXPONENT;
    if (bytes != PageSizeToBytes(page_size))
        page_size++; // extra page to get the overflow

    return page_size;
}

void NumaMemoryAllocator::HeapInit() {
    is_initialized_ = true;
#ifdef TEST_NUMA_LOGIC
    // set number of numa nodes to 8 to test numa allocation logic
    int num_numa_nodes = 8;
#else
    int num_numa_nodes = numaNodeCount();
#endif
    unsigned long node_mask = 0;
    for (unsigned long long node = 0; node < num_numa_nodes; node++)
    {
        node_mask = 0;
        node_mask |= (1 << node);
        numa_num_to_node.emplace_back(new NumaNode);
        void *new_chunk = SYS_MMAP_ALLOC(PageSizeToBytes(INIT_HEAP_PAGE_SIZE));
        if (!SYS_MMAP_CHECK(new_chunk)) {
            perror("NumaMemoryAllocator");
            FATAL("The memory allocator could not allocate memory");
        }
#if defined(USE_NUMA) && defined(MMAP_TOUCH_PAGES)
        // now bind it to the node and touch all the pages to make sure memory is bind to the node
        int retVal = mbind(new_chunk,                               // address
                           PageSizeToBytes(INIT_HEAP_PAGE_SIZE),    // length
                           MPOL_PREFERRED,                          // policy mode
                           &node_mask,                              // node mask
                           num_numa_nodes,                          // max number of nodes
                           MPOL_MF_MOVE);                           // policy mode flag
        ASSERT(retVal == 0);
        int* pInt = reinterpret_cast<int*>(new_chunk);
        for (unsigned int k = 0; k < PageSizeToBytes(INIT_HEAP_PAGE_SIZE)/4; k += (1 << (ALLOC_PAGE_SIZE_EXPONENT-2))) {
            pInt[k] = 0;
        }
#endif
        free_pages_ += INIT_HEAP_PAGE_SIZE;
        numa_num_to_node[node]->free_tree[INIT_HEAP_PAGE_SIZE].insert(new_chunk);

        ChunkInfo* tree_chunk = ChunkInfo::GetChunk(new_chunk, INIT_HEAP_PAGE_SIZE, node, false, nullptr, nullptr);
        ptr_to_bstchunk.emplace(new_chunk, tree_chunk);
    }
}

void* NumaMemoryAllocator::MmapAlloc(size_t num_bytes, int node, const char* f, int l) {
    if (0 == num_bytes)
        return nullptr;

    lock_guard<mutex> lck(mtx_);

    if (!is_initialized_)
        HeapInit();

    int num_pages = BytesToPageSize(num_bytes);
    if (kHashSegPageSize == num_pages) {
        return HashSegAlloc();
    }
    void* res_ptr = BSTreeAlloc(num_pages, node);
#if defined(USE_NUMA) || defined(TEST_NUMA_LOGIC)
    if (!res_ptr) {
        // if there is no fit chunk in current node, lookup other numa nodes
        for (int i = 0; i < numa_num_to_node.size(); i++) {
            if (i != node) {
                res_ptr = BSTreeAlloc(num_pages, i);
                if (res_ptr)
                    break;
            }
        }
    }
#endif
    if (!res_ptr) {
        GrowHeap(num_pages, node);
        res_ptr = BSTreeAlloc(num_pages, node);
    }
    return res_ptr;
}


void NumaMemoryAllocator::MmapChangeProt(void* ptr, int prot) {
    if (!ptr) {
        return;
    }

    lock_guard<mutex> lck(mtx_);
    if (occupied_hash_segs.find(ptr) != occupied_hash_segs.end()) {
        SYS_MMAP_PROT(ptr, kHashSegAlignedSize, prot);
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

void NumaMemoryAllocator::MmapFree(void* ptr) {
    if (!ptr)
        return;

    lock_guard<mutex> lck(mtx_);
    if (occupied_hash_segs.find(ptr) != occupied_hash_segs.end()) {
        reserved_hash_segs.push_back(ptr);
        occupied_hash_segs.erase(ptr);
        // UpdateStatus(-kHashSegPageSize);
    } else {
        FATALIF(ptr_to_bstchunk.find(ptr) == ptr_to_bstchunk.end(), "Freeing unallocated pointer %p.", ptr);
        BSTreeFree(ptr);
    }
}

void* NumaMemoryAllocator::HashSegAlloc() {
    void* res_ptr = nullptr;
    if (reserved_hash_segs.empty()) {
        // may use page aligned size
        res_ptr = SYS_MMAP_ALLOC(kHashSegAlignedSize);
        if (!SYS_MMAP_CHECK(res_ptr)){
            perror("NumaMemoryAllocator");
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

void NumaMemoryAllocator::EraseTreePtr(int size, void* ptr, int node) {
    auto it = numa_num_to_node[node]->free_tree.find(size);
    if (it->second.size() == 1)
        numa_num_to_node[node]->free_tree.erase(it);
    else
        it->second.erase(ptr);
}

void NumaMemoryAllocator::GrowHeap(int num_pages, int node) {
    int grow_pages = max(HEAP_GROW_BY_SIZE, num_pages);
    void* ptr = SYS_MMAP_ALLOC(PageSizeToBytes(grow_pages));
    FATALIF(!SYS_MMAP_CHECK(ptr),
            "Run out of memory in allocator. Request: %d MB", grow_pages / 2);
    ChunkInfo* new_chunk = ChunkInfo::GetChunk(ptr, grow_pages, node, false, nullptr, nullptr);
    ptr_to_bstchunk.emplace(ptr, new_chunk);
    numa_num_to_node[node]->free_tree[grow_pages].insert(ptr);
}

void* NumaMemoryAllocator::BSTreeAlloc(int num_pages, int node) {
    auto& cur_free_tree = numa_num_to_node[node]->free_tree;
    auto it = cur_free_tree.lower_bound(num_pages);
    if (it == cur_free_tree.end()) {
        return nullptr;
    } else {
        int size = it->first;
        unordered_set<void*>& ptrs = it->second;
        void* fit_ptr = *ptrs.begin();
        EraseTreePtr(size, fit_ptr, node);
        ChunkInfo* &alloc_chunk = ptr_to_bstchunk[fit_ptr];
        if (size > num_pages) {
            // if the selected free block is larger than the request size
            ChunkInfo* remain_chunk = alloc_chunk->Split(num_pages);
            cur_free_tree[remain_chunk->size].insert(remain_chunk->mem_ptr);
            ptr_to_bstchunk.emplace(remain_chunk->mem_ptr, remain_chunk);
        }
        UpdateStatus(num_pages);
        return alloc_chunk->mem_ptr;
    }
}

void NumaMemoryAllocator::BSTreeFree(void* ptr) {
    ChunkInfo* cur_chunk = ptr_to_bstchunk[ptr];
    int cur_node = cur_chunk->node;
    ptr_to_bstchunk.erase(ptr);
    cur_chunk->used = false;
    UpdateStatus(-cur_chunk->size);
    // coalesce with next chunk if it is free
    ChunkInfo* chunk = cur_chunk->CoalesceNext();
    if (chunk) {
        EraseTreePtr(chunk->size, chunk->mem_ptr, cur_node);
        ptr_to_bstchunk.erase(chunk->mem_ptr);
    }
    // coalesce with previous chunk if it is free
    chunk = cur_chunk->CoalescePrev();
    if (chunk) {
        EraseTreePtr(chunk->size, chunk->mem_ptr, cur_node);
        ptr_to_bstchunk.erase(chunk->mem_ptr);
    }
    numa_num_to_node[cur_node]->free_tree[cur_chunk->size].insert(cur_chunk->mem_ptr);
    ptr_to_bstchunk[cur_chunk->mem_ptr] = cur_chunk;
}

void NumaMemoryAllocator::UpdateStatus(int allocated_size) {
    free_pages_ -= allocated_size;
    allocated_pages_ += allocated_size;
}

size_t NumaMemoryAllocator::AllocatedPages() const {
    return allocated_pages_;
}

size_t NumaMemoryAllocator::FreePages() const {
    return free_pages_;
}

ChunkInfo::ChunkInfo(void* ptr, int s, int nd, bool u, ChunkInfo* p, ChunkInfo* n)
    : mem_ptr(ptr), size(s), node(nd), used(u), prev(p), next(n)
    { }

vector<ChunkInfo*> ChunkInfo::bstchunk_pool = vector<ChunkInfo*>();

ChunkInfo* ChunkInfo::GetChunk(void* ptr, int s, int nd, bool u, ChunkInfo* p, ChunkInfo* n) {
    ChunkInfo* chunk = nullptr;
    if (bstchunk_pool.empty()) {
        chunk = new ChunkInfo(ptr, s, nd, u, p, n);
    } else {
        chunk = bstchunk_pool.back();
        bstchunk_pool.pop_back();
        chunk->Assign(ptr, s, nd, u, p, n);
    }
    return chunk;
}

void ChunkInfo::PutChunk(ChunkInfo* chunk) {
    bstchunk_pool.push_back(chunk);
}

void ChunkInfo::FreeChunks() {
    for (auto c : bstchunk_pool) {
        delete c;
    }
}

void ChunkInfo::Assign(void* ptr, int s, int nd, bool u, ChunkInfo* p, ChunkInfo* n) {
    mem_ptr = ptr;
    size = s;
    node = nd;
    used = u;
    prev = p;
    next = n;
}

// get pointer that point to num_pages(convert to bytes) behind ptr
void* ChunkInfo::PtrSeek(void* ptr, int num_pages) {
    size_t num_bytes = static_cast<size_t>(num_pages) << ALLOC_PAGE_SIZE_EXPONENT;
    char* res = reinterpret_cast<char*>(ptr) + num_bytes;
    return reinterpret_cast<void*>(res);
}

// splits this chunk and return the remaining chunk
ChunkInfo* ChunkInfo::Split(int used_size) {
    assert(used_size <= size);
    void* remain = PtrSeek(mem_ptr, used_size);
    ChunkInfo* remain_chunk = GetChunk(remain, size - used_size, node, false, this, next);
    size = used_size;
    used = true;
    if (next) {
        next->prev = remain_chunk;
    }
    next = remain_chunk;
    return remain_chunk;
}

ChunkInfo* ChunkInfo::CoalescePrev() {
    // return false if no more coalesce can be perform
    if (!prev || prev->used) {
        return nullptr;
    }
    // pointing to the beginning of coalesced chunk
    mem_ptr = prev->mem_ptr;
    size += prev->size;
    PutChunk(prev);
    prev = prev->prev;
    if (prev) {
        // update next pointer of previous chunk
        prev->next = this;
    }
    return bstchunk_pool.back();
}

ChunkInfo* ChunkInfo::CoalesceNext() {
    // return false if no more coalesce can be perform
    if (!next || next->used) {
        return nullptr;
    }
    size += next->size;
    PutChunk(next);
    next = next->next;
    if (next) {
        // update previous pointer of next chunk
        next->prev = this;
    }
    return bstchunk_pool.back();
}

#ifdef GUNIT_TEST

ostream& operator <<(ostream &output, ChunkInfo &chunk) {
    output << "pointer:" << ((long)chunk.mem_ptr) / (512*1024) << endl;
    output << "size:" << chunk.size << endl;
    output << "node:" << chunk.node << endl;
    output << "used:" << (chunk.used ? "true" : "false") << endl;
    output << "prev:" << (chunk.prev ? (long)chunk.prev->mem_ptr / (512*1024) : 0) << endl;
    output << "next:" << (chunk.next ? (long)chunk.next->mem_ptr / (512*1024) : 0) << endl;
    return output;
}

#endif
