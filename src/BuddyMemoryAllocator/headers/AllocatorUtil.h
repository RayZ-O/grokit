#ifndef _ALLOCATOR_UTIL_H_
#define _ALLOCATOR_UTIL_H_

size_t PageSizeToBytes(int page_size);

int BytesToPageSize(size_t bytes);

// get pointer that point to num_pages(convert to bytes) behind ptr
void* PtrSeek(void* ptr, int num_pages);

#endif
