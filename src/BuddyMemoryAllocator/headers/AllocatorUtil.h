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
#ifndef _ALLOCATOR_UTIL_H_
#define _ALLOCATOR_UTIL_H_

#include <cassert>

size_t PageSizeToBytes(int page_size);

int BytesToPageSize(size_t bytes);

// get pointer that point to num_pages(convert to bytes) behind ptr
void* PtrSeek(void* ptr, int num_pages);

#endif
