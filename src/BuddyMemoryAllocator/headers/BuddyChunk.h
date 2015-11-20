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
#ifndef _BUDDY_CHUNK_H_
#define _BUDDY_CHUNK_H_

#define GUNIT_TEST 1

#include <vector>

class BuddyChunk {
public:
    void* mem_ptr;
    int size;      // size of the chunk, may be larger than the request size(internal fragment)
    bool used;
    int order;     // TODO size is not necessary if order is stored
    int page_index;  // offset from mem_ptr to buddy system base pointer in page size

    BuddyChunk(void* ptr, int s, bool u, int o, int i);

    void Assign(void* ptr, int s, bool u, int o, int i);

    // gets buddy chunk from chunk pool, if the pool is empty, allocate a new one
    static BuddyChunk* GetChunk(void* ptr, int size, bool used, int order, int idx);
    // puts buddy chunk back to chunk pool
    static void PutChunk(BuddyChunk* chunk);

#ifdef GUNIT_TEST
    friend std::ostream& operator <<(std::ostream &output, BuddyChunk &chunk);
#endif

private:
    // object pool for buddy chunk to avoid frequently new and delete
    static std::vector<BuddyChunk*> budchunk_pool;
};

#endif
