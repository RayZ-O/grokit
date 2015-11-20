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
#include "MmapAllocator.h"

size_t PageSizeToBytes(int page_size) {
    return static_cast<size_t>(page_size) << ALLOC_PAGE_SIZE_EXPONENT;
}

int BytesToPageSize(size_t bytes) {
    // compute the size in pages
    int page_size = bytes >> ALLOC_PAGE_SIZE_EXPONENT;
    if (bytes != PageSizeToBytes(page_size))
        page_size++; // extra page to get the overflow

    return page_size;
}

// get pointer that point to num_pages(convert to bytes) behind ptr
void* PtrSeek(void* ptr, int num_pages) {
    char* res = reinterpret_cast<char*>(ptr) + PageSizeToBytes(num_pages);
    return reinterpret_cast<void*>(res);
}
