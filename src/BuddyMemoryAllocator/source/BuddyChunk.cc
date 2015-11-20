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
#include "BuddyChunk.h"

using namespace std;

BuddyChunk::BuddyChunk(void* ptr, int s, bool u, int o, int i)
    : mem_ptr(ptr),
      size(s),
      used(u),
      order(o),
      page_index(i)
    { }

vector<BuddyChunk*> BuddyChunk::budchunk_pool = vector<BuddyChunk*>();

BuddyChunk* BuddyChunk::GetChunk(void* ptr, int size, bool used, int order, int idx) {
    BuddyChunk* chunk = nullptr;
    if (budchunk_pool.empty())
        chunk = new BuddyChunk(ptr, size, used, order, idx);
    else {
        chunk = budchunk_pool.back();
        budchunk_pool.pop_back();
        chunk->Assign(ptr, size, used, order, idx);
    }
    return chunk;
}

void BuddyChunk::PutChunk(BuddyChunk* chunk) {
    budchunk_pool.push_back(chunk);
}

void BuddyChunk::Assign(void* ptr, int s, bool u, int o, int i) {
    mem_ptr = ptr;
    size = s;
    used = u;
    order = o;
    page_index = i;
}

#ifdef GUNIT_TEST

ostream& operator <<(ostream &output, BuddyChunk &chunk) {
    output << "pointer:" << ((long)chunk.mem_ptr) / (512*1024) << endl;
    output << "size:" << chunk.size << endl;
    output << "used:" << (chunk.used ? "true" : "false") << endl;
    output << "order:" << chunk.order << endl;
    output << "page index:" << chunk.page_index << endl;
    return output;
}

#endif
