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

void* mmap_alloc_imp(size_t noBytes, int node, const char* f, int l){
    BuddyMemoryAllocator& aloc=BuddyMemoryAllocator::GetAllocator();
    void* rez= aloc.MmapAlloc(noBytes, node, f, l);
    return rez;
}

void mmap_prot_read_imp(void* ptr, const char* f, int l){
    BuddyMemoryAllocator& aloc=BuddyMemoryAllocator::GetAllocator();
    aloc.MmapChangeProt(ptr, PROT_READ);
}

void mmap_prot_readwrite_imp(void* ptr, const char* f, int l){
    BuddyMemoryAllocator& aloc=BuddyMemoryAllocator::GetAllocator();
    aloc.MmapChangeProt(ptr, PROT_READ | PROT_WRITE);
}

void mmap_free_imp(void* ptr, const char* f, int l){
    if( ptr == NULL ) {
        WARNING("Warning: Attempted free of null pointer at %s:%d", f, l);
    }

    BuddyMemoryAllocator& aloc=BuddyMemoryAllocator::GetAllocator();
    aloc.MmapFree(ptr);
}

off_t mmap_used(void){
    BuddyMemoryAllocator& aloc=BuddyMemoryAllocator::GetAllocator();
    return PAGES_TO_BYTES(aloc.AllocatedPages());
}

BuddyMemoryAllocator::BuddyMemoryAllocator(void) {
    // initialize the mutex
    mHeapInitialized = false;
}


BuddyMemoryAllocator::~BuddyMemoryAllocator(void) {

}

int BuddyMemoryAllocator::BytesToPageSize(size_t bytes){
    // compute the size in pages
    int pSize = bytes >> ALLOC_PAGE_SIZE_EXPONENT;
    if (bytes != PageSizeToBytes(pSize) )
        pSize++; // extra page to get the overflow

    return pSize;
}

size_t BuddyMemoryAllocator::PageSizeToBytes(int pSize){
    return ((size_t) pSize) << ALLOC_PAGE_SIZE_EXPONENT;
}

void BuddyMemoryAllocator::HeapInit()
{

}

void* BuddyMemoryAllocator::MmapAlloc(size_t noBytes, int node, const char* f, int l){
    if (noBytes == 0)
        return NULL;


    if (!mHeapInitialized)
        HeapInit();


    return rezPtr;
}


void BuddyMemoryAllocator::MmapChangeProt(void* ptr, int prot) {
    if (ptr==NULL) {
        return;
    }
}

void BuddyMemoryAllocator::MmapFree(void* ptr){
    if (ptr==NULL) {
        return;
    }


}

size_t BuddyMemoryAllocator::AllocatedPages() {
    size_t size = 0;

    return size;
}

size_t BuddyMemoryAllocator::FreePages() {
    size_t totalFreelistSize = 0;

    return totalFreelistSize;
}

