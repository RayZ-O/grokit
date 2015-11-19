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
