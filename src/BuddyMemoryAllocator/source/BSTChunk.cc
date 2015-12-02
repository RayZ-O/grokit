//
//  Copyright 2015 Rui Zhang
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
#include <iostream>
#include "BSTChunk.h"
#include "AllocatorUtil.h"

using namespace std;

BSTreeChunk::BSTreeChunk(void* ptr, int s, bool u, BSTreeChunk* p, BSTreeChunk* n)
    : mem_ptr(ptr),
      size(s),
      used(u),
      prev(p),
      next(n)
    { }

vector<BSTreeChunk*> BSTreeChunk::bstchunk_pool = vector<BSTreeChunk*>();

BSTreeChunk* BSTreeChunk::GetChunk(void* ptr, int s, bool u, BSTreeChunk* p, BSTreeChunk* n) {
    BSTreeChunk* chunk = nullptr;
    if (bstchunk_pool.empty()) {
        chunk = new BSTreeChunk(ptr, s, u, p, n);
    } else {
        chunk = bstchunk_pool.back();
        bstchunk_pool.pop_back();
        chunk->Assign(ptr, s, u, p, n);
    }
    return chunk;
}

void BSTreeChunk::PutChunk(BSTreeChunk* chunk) {
    bstchunk_pool.push_back(chunk);
}

void BSTreeChunk::Assign(void* ptr, int s, bool u, BSTreeChunk* p, BSTreeChunk* n) {
    mem_ptr = ptr;
    size = s;
    used = u;
    prev = p;
    next = n;
}

// splits this chunk and return the remaining chunk
BSTreeChunk* BSTreeChunk::Split(int used_size) {
    assert(used_size <= size);
    void* remain = PtrSeek(mem_ptr, used_size);
    BSTreeChunk* remain_chunk = GetChunk(remain,             // pointer to the beginning of remaining part
                                         size - used_size,   // size of the chunk
                                         false,              // is in used
                                         this,               // previous physical chunk
                                         next);              // next physical chunk
    size = used_size;
    used = true;
    next = remain_chunk;
    return remain_chunk;
}

pair<BSTreeChunk*, bool> BSTreeChunk::CoalescePrev() {
    // return false if no more coalesce can be perform
    if (!prev || prev->used) {
        return {nullptr, false};
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
    return {bstchunk_pool.back(), true};
}

pair<BSTreeChunk*, bool> BSTreeChunk::CoalesceNext() {
    // return false if no more coalesce can be perform
    if (!next || next->used) {
        return {nullptr, false};
    }
    size += next->size;
    PutChunk(next);
    next = next->next;
    if (next) {
        // update previous pointer of next chunk
        next->prev = this;
    }
    return {bstchunk_pool.back(), true};
}

#ifdef GUNIT_TEST

ostream& operator <<(ostream &output, BSTreeChunk &chunk) {
    output << "pointer:" << ((long)chunk.mem_ptr) / (512*1024) << endl;
    output << "size:" << chunk.size << endl;
    output << "used:" << (chunk.used ? "true" : "false") << endl;
    output << "prev:" << (chunk.prev ? (long)chunk.prev->mem_ptr / (512*1024) : 0) << endl;
    output << "next:" << (chunk.next ? (long)chunk.next->mem_ptr / (512*1024) : 0) << endl;
    return output;
}

#endif
