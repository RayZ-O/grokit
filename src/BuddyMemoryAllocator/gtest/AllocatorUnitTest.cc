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

#include <gtest/gtest.h>
#include <iostream>
#include "../headers/BuddyMemoryAllocator.h"

using namespace std;

constexpr size_t operator "" _page(unsigned long long page_size) {
    return page_size << ALLOC_PAGE_SIZE_EXPONENT;
}

size_t PageToBytes(int page_size) {
    return static_cast<size_t>(page_size) << ALLOC_PAGE_SIZE_EXPONENT;
}

int subtract(void* ptr1, void* ptr2) {
    return reinterpret_cast<char*>(ptr1) - reinterpret_cast<char*>(ptr2);
}

class AllocatorTest : public ::testing::Test {
protected:
    static constexpr int kBuddyHeapSize = 1 << MAX_ORDER;
    static constexpr int kBSTHeapSize = INIT_HEAP_PAGE_SIZE - (1 << MAX_ORDER);
    BuddyMemoryAllocator aloc;
    int node;
    char* filename;
    int linenum;

    AllocatorTest() : aloc() {
        node = 0;
        filename = nullptr;
        linenum = 0;
    }

    virtual void SetUp() {
        // Code here will be called immediately after the constructor (right
        // before each test).
    }

    virtual void TearDown() {
        // Code here will be called right before the destructor.

    }

    // write the tests as members of the fixture class to access private members,
    // since even though this test fixture is a friend to allocator class, the
    // tests(sub-classes of the fixture) are not automatically friends to it
    void AllocateZeroTest();
    void HashSegTest();
    // buddy system tests
    void GetBuddyOrderTest();
    void BuddySplitTest();
    void BuddyCoalesceTest();
    void BuddyFixedSizeTest();
    void BuddySmallToLargeTest();
    void BuddyLargeToSmallTest();
    void BuddyIncreasingFillTest();
    void BuddyDecreasingFillTest();
    void BuddyVariousFillTest();
    void BuddyFreeHalfRefillTest();
    // binary tree syetem tests
    void BstAllocateTest();
    void BstFreeTest();
    // TODO more bst tests
    void HybridAllocateTest();
    // pretty print the internal data structure infomation
    void PrintBuddyPtrMap();
    void PrintBSTPtrMap();
    void PrintFreeArea();
    void PrintFreeTree();
    // use the internal data structure to calculate allocated and free pages
    int GetAllocatedBuddySize();
    int GetFreeBuddySize();
    int GetAllocatedBSTSize();
    int GetFreeBSTSize();
};

constexpr int AllocatorTest::kBuddyHeapSize;
constexpr int AllocatorTest::kBSTHeapSize;

void AllocatorTest::PrintBuddyPtrMap() {
    for (const auto p : aloc.ptr_to_budchunk) {
        cout << *p.second << endl;
    }
}

void AllocatorTest::PrintBSTPtrMap() {
    for (const auto p : aloc.ptr_to_bstchunk) {
        cout << *p.second << endl;
    }
}

void AllocatorTest::PrintFreeArea() {
    for (int i = 0; i < aloc.free_area.size(); i++) {
        cout << "order " << i << ": ";
        for (int idx : aloc.free_area[i]) {
            cout << idx << " ";
        }
        cout << endl;
    }
}

void AllocatorTest::PrintFreeTree() {
    for (const auto p : aloc.free_tree) {
        cout << "size in tree: " << p.first << endl;
        for (const auto ptr : p.second) {
            cout << aloc.ptr_to_bstchunk[ptr] << endl;
        }
        cout << endl;
    }
}

int AllocatorTest::GetAllocatedBuddySize() {
    int size = 0;
    for (const auto p : aloc.ptr_to_budchunk) {
        if (p.second->used)
            size += p.second->size;
    }
    return size;
}

int AllocatorTest::GetFreeBuddySize() {
    int size = 0;
    for (int i = 0; i < aloc.free_area.size(); i++) {
        size += aloc.free_area[i].size() * (1 << i);
    }
    return size;
}

int AllocatorTest::GetAllocatedBSTSize() {
    int size = 0;
    for (const auto p : aloc.ptr_to_bstchunk) {
        if (p.second->used)
            size += p.second->size;
    }
    return size;
}

int AllocatorTest::GetFreeBSTSize() {
    int size = 0;
    for (const auto p : aloc.free_tree) {
        size += p.first * p.second.size();
    }
    return size;
}

void AllocatorTest::AllocateZeroTest() {
    EXPECT_TRUE(aloc.MmapAlloc(0_page, node, filename, linenum) == nullptr);
}

void AllocatorTest::HashSegTest() {
    void* ptr1 = aloc.MmapAlloc(PageToBytes(aloc.kHashSegPageSize), node, filename, linenum);
    EXPECT_TRUE(ptr1 != nullptr);
    EXPECT_EQ(1, aloc.occupied_hash_segs.size());
    EXPECT_EQ(0, aloc.reserved_hash_segs.size());
    aloc.MmapFree(ptr1);
    EXPECT_EQ(0, aloc.occupied_hash_segs.size());
    EXPECT_EQ(1, aloc.reserved_hash_segs.size());
    EXPECT_EQ(0, aloc.AllocatedPages());
}

void AllocatorTest::GetBuddyOrderTest() {
    EXPECT_EQ(0, aloc.GetOrder(1));
    EXPECT_EQ(1, aloc.GetOrder(2));
    EXPECT_EQ(2, aloc.GetOrder(3));
    EXPECT_EQ(2, aloc.GetOrder(4));
    EXPECT_EQ(3, aloc.GetOrder(7));
    EXPECT_EQ(4, aloc.GetOrder(16));
    EXPECT_EQ(5, aloc.GetOrder(29));
    EXPECT_EQ(6, aloc.GetOrder(40));
    EXPECT_EQ(8, aloc.GetOrder(254));
}

void AllocatorTest::BuddySplitTest() {
    vector<void*> ptrs;
    ptrs.push_back(aloc.MmapAlloc(1_page, node, filename, linenum));
    // after allocating 1 page, each free list should have 1 element
    for (int i = 0; i < MAX_ORDER; i++) {
        EXPECT_EQ(1, aloc.free_area[i].size());
    }
    ptrs.push_back(aloc.MmapAlloc(4_page, node, filename, linenum));
    EXPECT_EQ(0, aloc.free_area[2].size());
    // allocate 4 pages(order 2) twice, 8 pages(order 3) block will be splitted
    ptrs.push_back(aloc.MmapAlloc(4_page, node, filename, linenum));
    EXPECT_EQ(1, aloc.free_area[2].size());
    EXPECT_EQ(0, aloc.free_area[3].size());
    ptrs.push_back(aloc.MmapAlloc(8_page, node, filename, linenum));
    EXPECT_EQ(1, aloc.free_area[3].size());
    EXPECT_EQ(0, aloc.free_area[4].size());
    EXPECT_EQ(aloc.AllocatedPages(), GetAllocatedBuddySize());
    for (auto p : ptrs) {
        aloc.MmapFree(p);
    }
    EXPECT_EQ(0, GetAllocatedBuddySize());
    EXPECT_EQ(kBuddyHeapSize, GetFreeBuddySize());
}

void AllocatorTest::BuddyCoalesceTest() {
    void* ptr_2p = aloc.MmapAlloc(2_page, node, filename, linenum);
    for (int i = 1; i < MAX_ORDER; i++) {
        EXPECT_EQ(1, aloc.free_area[i].size());
    }
    aloc.MmapFree(ptr_2p);
    // after free all free block coalesce to largest block
    EXPECT_EQ(1, aloc.free_area[MAX_ORDER].size());
    void* ptr_8p = aloc.MmapAlloc(8_page, node, filename, linenum);
    // allocate 4 pages to split buddy block
    void* ptr_4p = aloc.MmapAlloc(4_page, node, filename, linenum);
    aloc.MmapFree(ptr_8p);
    // size of buddy is 4, no coalesce
    EXPECT_EQ(1, aloc.free_area[3].size());
    aloc.MmapFree(ptr_4p);
    EXPECT_EQ(1, aloc.free_area[MAX_ORDER].size());
    ptr_8p = aloc.MmapAlloc(8_page, node, filename, linenum);
    void* ptr_8p2 = aloc.MmapAlloc(8_page, node, filename, linenum);
    aloc.MmapFree(ptr_8p);
    // buddy in use, no coalesce
    EXPECT_EQ(1, aloc.free_area[3].size());
    aloc.MmapFree(ptr_8p2);
    EXPECT_EQ(1, aloc.free_area[MAX_ORDER].size());
}

void AllocatorTest::BuddyFixedSizeTest() {
    // repeatedly allocate fixed size pages
    for (int size = 1, order = 0; size < kBuddyHeapSize; size <<= 1, order++) {
    // for (int size = 64, order = 6; size < kBuddyHeapSize; size <<= 1, order++) {
        vector<void*> ptrs;
        for (int i = 1; i < kBuddyHeapSize / size; i++) {
            ptrs.push_back(aloc.MmapAlloc(PageToBytes(size), node, filename, linenum));
            EXPECT_TRUE(ptrs.back() != nullptr);
            EXPECT_EQ(i * size, aloc.AllocatedPages());
            EXPECT_EQ(aloc.AllocatedPages(), GetAllocatedBuddySize());
            EXPECT_EQ(i % 2, aloc.free_area[order].size());
        }
        EXPECT_EQ(size , GetFreeBuddySize());
        for (auto p : ptrs) {
            aloc.MmapFree(p);
        }
        EXPECT_EQ(0, GetAllocatedBuddySize());
        EXPECT_EQ(kBuddyHeapSize, GetFreeBuddySize());
        EXPECT_EQ(1, aloc.free_area[MAX_ORDER].size());
    }
}

void AllocatorTest::BuddySmallToLargeTest() {
    vector<void*> ptrs;
    ptrs.push_back(aloc.MmapAlloc(1_page, node, filename, linenum));
    int num_ptrs = aloc.ptr_to_budchunk.size();
    for (int size = 2, i = 1; size < kBuddyHeapSize; size <<= 1, i++) {
        ptrs.push_back(aloc.MmapAlloc(PageToBytes(size), node, filename, linenum));
        EXPECT_TRUE(ptrs.back() != nullptr);
        EXPECT_EQ(0, aloc.free_area[i].size());
        EXPECT_EQ(num_ptrs, aloc.ptr_to_budchunk.size());
    }
    EXPECT_EQ(kBuddyHeapSize - 1, GetAllocatedBuddySize());
    EXPECT_EQ(1, GetFreeBuddySize());
    for (auto p : ptrs) {
        aloc.MmapFree(p);
    }
    EXPECT_EQ(0, GetAllocatedBuddySize());
    EXPECT_EQ(kBuddyHeapSize, GetFreeBuddySize());
}

void AllocatorTest::BuddyLargeToSmallTest() {
    vector<void*> ptrs;
    int num_ptrs = 1;
    for (int size = kBuddyHeapSize >> 1, i = MAX_ORDER - 1; size >= 2; size >>= 1, i--) {
        ptrs.push_back(aloc.MmapAlloc(PageToBytes(size), node, filename, linenum));
        EXPECT_TRUE(ptrs.back() != nullptr);
        EXPECT_EQ(1, aloc.free_area[i].size());
        EXPECT_EQ(++num_ptrs, aloc.ptr_to_budchunk.size());
    }
    EXPECT_EQ(kBuddyHeapSize - 2, GetAllocatedBuddySize());
    EXPECT_EQ(2, GetFreeBuddySize());
    for (auto p : ptrs) {
        aloc.MmapFree(p);
    }
    EXPECT_EQ(0, GetAllocatedBuddySize());
    EXPECT_EQ(kBuddyHeapSize, GetFreeBuddySize());
}

void AllocatorTest::BuddyIncreasingFillTest() {
    vector<void*> ptrs;
    int tot_size = 0;
    // allocate increasing sequence
    for (int p_size : {1, 1, 2, 3, 5, 8, 13, 21}) {
        ptrs.push_back(aloc.MmapAlloc(PageToBytes(p_size), node, filename, linenum));
        EXPECT_TRUE(ptrs.back() != nullptr);
        tot_size += 1 << aloc.GetOrder(p_size);
        EXPECT_EQ(tot_size, GetAllocatedBuddySize());
    }
    for (int i = 0; i < 3; i++) {
        EXPECT_EQ(0, aloc.free_area[i].size());
        EXPECT_EQ(1, aloc.free_area[i + 3].size());
    }
    for (auto p : ptrs) {
        aloc.MmapFree(p);
    }
    EXPECT_EQ(0, GetAllocatedBuddySize());
    EXPECT_EQ(kBuddyHeapSize, GetFreeBuddySize());
}

void AllocatorTest::BuddyDecreasingFillTest() {
    vector<void*> ptrs;
    int tot_size = 0;
    // allocate decreasing sequence
    for (int p_size : {25, 16, 9, 4, 1}) {
        ptrs.push_back(aloc.MmapAlloc(PageToBytes(p_size), node, filename, linenum));
        EXPECT_TRUE(ptrs.back() != nullptr);
        tot_size += 1 << aloc.GetOrder(p_size);
        EXPECT_EQ(tot_size, GetAllocatedBuddySize());
    }
    EXPECT_EQ(1, aloc.free_area[0].size());
    EXPECT_EQ(1, aloc.free_area[1].size());
    EXPECT_EQ(1, aloc.free_area[3].size());
    EXPECT_EQ(1, aloc.free_area[4].size());
    EXPECT_EQ(1, aloc.free_area[5].size());
    EXPECT_EQ(0, aloc.free_area[2].size());
    EXPECT_EQ(0, aloc.free_area[6].size());
    for (auto p : ptrs) {
        aloc.MmapFree(p);
    }
    EXPECT_EQ(0, GetAllocatedBuddySize());
    EXPECT_EQ(kBuddyHeapSize, GetFreeBuddySize());
}

void AllocatorTest::BuddyVariousFillTest() {
    vector<void*> ptrs;
    int tot_size = 0;
    for (int p_size : {7, 3, 2, 5, 14, 50, 15, 3}) {
        ptrs.push_back(aloc.MmapAlloc(PageToBytes(p_size), node, filename, linenum));
        EXPECT_TRUE(ptrs.back() != nullptr);
        tot_size += 1 << aloc.GetOrder(p_size);
        EXPECT_EQ(tot_size, GetAllocatedBuddySize());
    }
    EXPECT_EQ(1, aloc.free_area[1].size());
    EXPECT_EQ(1, aloc.free_area[2].size());
    EXPECT_EQ(0, aloc.free_area[0].size());
    EXPECT_EQ(0, aloc.free_area[3].size());
    EXPECT_EQ(0, aloc.free_area[4].size());
    EXPECT_EQ(0, aloc.free_area[5].size());
    EXPECT_EQ(0, aloc.free_area[6].size());
    for (auto p : ptrs) {
        aloc.MmapFree(p);
    }
    EXPECT_EQ(0, GetAllocatedBuddySize());
    EXPECT_EQ(kBuddyHeapSize, GetFreeBuddySize());
}

void AllocatorTest::BuddyFreeHalfRefillTest() {
    vector<void*> ptrs;
    vector<int> sizes{7, 3, 2, 5, 14, 50, 15, 3};
    int tot_size = 0;
    for (int p_size : sizes) {
        ptrs.push_back(aloc.MmapAlloc(PageToBytes(p_size), node, filename, linenum));
        EXPECT_TRUE(ptrs.back() != nullptr);
        tot_size += 1 << aloc.GetOrder(p_size);
    }
    // free half of allocated blocks
    for (int i = 0; i < ptrs.size(); i++) {
        if (i % 2) {
            aloc.MmapFree(ptrs[i]);
            ptrs[i] = nullptr;
            tot_size -= 1 << aloc.GetOrder(sizes[i]);
            EXPECT_EQ(tot_size, GetAllocatedBuddySize());
        }
    }
    for (int p_size : {2, 31, 4, 7}) {
        ptrs.push_back(aloc.MmapAlloc(PageToBytes(p_size), node, filename, linenum));
        EXPECT_TRUE(ptrs.back() != nullptr);
        tot_size += 1 << aloc.GetOrder(p_size);
        EXPECT_EQ(tot_size, GetAllocatedBuddySize());
    }
    EXPECT_EQ(0, aloc.free_area[0].size());
    EXPECT_EQ(0, aloc.free_area[1].size());
    EXPECT_EQ(0, aloc.free_area[2].size());
    EXPECT_EQ(1, aloc.free_area[3].size());
    EXPECT_EQ(0, aloc.free_area[4].size());
    EXPECT_EQ(1, aloc.free_area[5].size());
    EXPECT_EQ(0, aloc.free_area[6].size());
    for (auto p : ptrs) {
        aloc.MmapFree(p);
    }
    EXPECT_EQ(0, GetAllocatedBuddySize());
    EXPECT_EQ(kBuddyHeapSize, GetFreeBuddySize());
}

void AllocatorTest::BstAllocateTest() {
    vector<void*> ptrs;
    for (int i = 1; i <= 3; i++) {
        ptrs.push_back(aloc.MmapAlloc(PageToBytes(kBuddyHeapSize + i * 10), node, filename, linenum));
        EXPECT_TRUE(ptrs.back() != nullptr);
        EXPECT_EQ(i + 1, aloc.ptr_to_bstchunk.size());
    }
    EXPECT_EQ(aloc.AllocatedPages(), GetAllocatedBSTSize());
    EXPECT_EQ(kBSTHeapSize - aloc.AllocatedPages(), GetFreeBSTSize());
    // check sibling list
    for (const auto p : aloc.ptr_to_bstchunk) {
        if (p.second->next)
            EXPECT_EQ(PageToBytes(p.second->size), subtract(p.second->next->mem_ptr, p.second->mem_ptr));
        if (p.second->prev)
            EXPECT_EQ(PageToBytes(p.second->prev->size), subtract(p.second->mem_ptr, p.second->prev->mem_ptr));
    }
    for (auto p : ptrs) {
        aloc.MmapFree(p);
    }
    EXPECT_EQ(0, GetAllocatedBSTSize());
    EXPECT_EQ(kBSTHeapSize, GetFreeBSTSize());
}

void AllocatorTest::BstFreeTest() {
    vector<void*> ptrs;
    for (int i = 1; i <= 3; i++) {
        ptrs.push_back(aloc.MmapAlloc(PageToBytes(kBuddyHeapSize + i * 10), node, filename, linenum));
        EXPECT_TRUE(ptrs.back() != nullptr);
        EXPECT_EQ(i + 1, aloc.ptr_to_bstchunk.size());
    }
    aloc.MmapFree(ptrs[1]);
    EXPECT_EQ(4, aloc.ptr_to_bstchunk.size());
    aloc.MmapFree(ptrs[2]);
    EXPECT_EQ(2, aloc.ptr_to_bstchunk.size());
    aloc.MmapFree(ptrs[0]);
    EXPECT_EQ(1, aloc.ptr_to_bstchunk.size());
    EXPECT_EQ(0, GetAllocatedBSTSize());
    EXPECT_EQ(kBSTHeapSize, GetFreeBSTSize());
}

void AllocatorTest::HybridAllocateTest() {
    vector<void*> ptrs;
    ptrs.push_back(aloc.MmapAlloc(PageToBytes(kBuddyHeapSize - 1), node, filename, linenum));
    ptrs.push_back(aloc.MmapAlloc(2_page, node, filename, linenum));
    EXPECT_EQ(2, aloc.ptr_to_bstchunk.size());
    EXPECT_EQ(aloc.AllocatedPages(), GetAllocatedBuddySize() + GetAllocatedBSTSize());
    aloc.MmapFree(ptrs[0]);
    ptrs[0] = aloc.MmapAlloc(2_page, node, filename, linenum);
    EXPECT_EQ(2, aloc.ptr_to_bstchunk.size());
    for (auto p : ptrs) {
        aloc.MmapFree(p);
    }
    EXPECT_EQ(0, GetAllocatedBSTSize());
    EXPECT_EQ(kBSTHeapSize, GetFreeBSTSize());
}

TEST_F(AllocatorTest, AllocateZeroTest) { AllocateZeroTest(); }
TEST_F(AllocatorTest, HashSegTest) { HashSegTest(); }
TEST_F(AllocatorTest, GetBuddyOrderTest) { GetBuddyOrderTest(); }
TEST_F(AllocatorTest, BuddyFixedSizeTest) { BuddyFixedSizeTest(); }
TEST_F(AllocatorTest, BuddySplitTest) { BuddySplitTest(); }
TEST_F(AllocatorTest, BuddyCoalesceTest) { BuddyCoalesceTest(); }
TEST_F(AllocatorTest, BuddySmallToLargeTest) { BuddySmallToLargeTest(); }
TEST_F(AllocatorTest, BuddyLargeToSmallTest) { BuddyLargeToSmallTest(); }
TEST_F(AllocatorTest, BuddyIncreasingFillTest) { BuddyIncreasingFillTest(); }
TEST_F(AllocatorTest, BuddyDecreasingFillTest) { BuddyDecreasingFillTest(); }
TEST_F(AllocatorTest, BuddyVariousFillTest) { BuddyVariousFillTest(); }
TEST_F(AllocatorTest, BuddyFreeHalfRefillTest) { BuddyFreeHalfRefillTest(); }
TEST_F(AllocatorTest, BstAllocateTest) { BstAllocateTest(); }
TEST_F(AllocatorTest, BstFreeTest) { BstFreeTest(); }
TEST_F(AllocatorTest, HybridAllocateTest) { HybridAllocateTest(); }

int main(int argc, char **argv) {
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
