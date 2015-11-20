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
#ifndef _BST_CHUNK_H_
#define _BST_CHUNK_H_

#define GUNIT_TEST 1

#include <vector>

class BSTreeChunk {
public:
    void* mem_ptr;
    int size;
    bool used;
    BSTreeChunk* prev; // pointer to previous physical chunk
    BSTreeChunk* next; // pointer to next physical chunk

    BSTreeChunk(void* ptr, int s, bool u, BSTreeChunk* p, BSTreeChunk* n);
    // deletes cpoy constructor
    BSTreeChunk(const BSTreeChunk& other) = delete;
    // deletes copy assignment operator
    BSTreeChunk& operator = (const BSTreeChunk& other) = delete;
    // assigns values to the chunk
    void Assign(void* ptr, int s, bool u, BSTreeChunk* p, BSTreeChunk* n);
    // splits current chunk into two chunks of new_size and size - new_size
    BSTreeChunk* Split(int new_size);
    // coalesces with the previous chunk
    std::pair<BSTreeChunk*, bool> CoalescePrev();
    // coalesces with the next chunk
    std::pair<BSTreeChunk*, bool> CoalesceNext();
    // gets chunk from chunk pool
    static BSTreeChunk* GetChunk(void* ptr, int s, bool u, BSTreeChunk* p, BSTreeChunk* n);
    // puts tree chunk back to chunk pool
    static void PutChunk(BSTreeChunk* chunk);

#ifdef GUNIT_TEST
    // override ostream operator for pretty print
    friend std::ostream& operator <<(std::ostream &output, BSTreeChunk &chunk);
#endif

private:
    static std::vector<BSTreeChunk*> bstchunk_pool;  // chunk pool
};

#endif
