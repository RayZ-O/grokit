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
#include "../headers/NumaMemoryAllocator.h"
#include "../headers/MmapAllocator.h"

using namespace std;

constexpr size_t operator "" _page(unsigned long long page_size) {
    return page_size << ALLOC_PAGE_SIZE_EXPONENT;
}

int subtract(void* ptr1, void* ptr2) {
    return reinterpret_cast<char*>(ptr1) - reinterpret_cast<char*>(ptr2);
}

class AllocatorTest : public ::testing::Test {
protected:
    static constexpr int kBSTHeapSize = INIT_HEAP_PAGE_SIZE;
    BuddyMemoryAllocator& aloc;
    char* filename;
    int linenum;

    AllocatorTest() : aloc(BuddyMemoryAllocator::GetAllocator()) {
        filename = nullptr;
        linenum = 0;
    }

    virtual void SetUp() {

    }

    virtual void TearDown() {
        EXPECT_EQ(0, mmap_used());
    }

    // write the tests as members of the fixture class to access private members,
    // since even though this test fixture is a friend to allocator class, the
    // tests(sub-classes of the fixture) are not automatically friends to it
    void AllocateZeroTest();
    void HashSegTest();
    // binary tree allocator tests
    void BstFreePagesTest();
    void BstChangeProtTest();
    void BstChunkPointerTest();
    void BstCoalesceTest();
    void BstFixedSizeTest();
    void BstSmallToLargeTest();
    void BstLargeToSmallTest();
    void BstIncreasingFillTest();
    void BstDecreasingFillTest();
    void BstVariousFillTest();
    void BstFreeHalfRefillTest1();
    void BstFreeHalfRefillTest2();
    void BstFreeOneThirdRefillTest();
    void MultiNodeAllocateTest();
    void MultiNodeFreeTest();
    void BstHeapGrowTest();
    // pretty print the internal data structure infomation
    void PrintBSTPtrMap();
    void PrintFreeTree(int node);
    // use the internal data structure to calculate allocated and free pages
    int GetAllocatedBSTSize();
    int GetFreeBSTSize(int node);
    int GetTreeSize(int node);
};

constexpr int AllocatorTest::kBSTHeapSize;

void AllocatorTest::PrintBSTPtrMap() {
    cout << "------------Ptr to BST-----------------" << endl;
    for (const auto p : aloc.ptr_to_bstchunk) {
        cout << *p.second << endl;
    }
    cout << "---------------------------------------" << endl;
}

void AllocatorTest::PrintFreeTree(int node) {
    cout << "-------------Free Tree-----------------" << endl;
    for (const auto p : aloc.numa_num_to_node[node]->free_tree) {
        cout << "size in tree: " << p.first << endl;
        for (const auto ptr : p.second) {
            cout << *aloc.ptr_to_bstchunk[ptr] << endl;
        }
        cout << endl;
    }
    cout << "---------------------------------------" << endl;
}

int AllocatorTest::GetAllocatedBSTSize() {
    int size = 0;
    for (const auto p : aloc.ptr_to_bstchunk) {
        if (p.second->used)
            size += p.second->size;
    }
    return size;
}

int AllocatorTest::GetFreeBSTSize(int node) {
    int size = 0;
    for (const auto p : aloc.numa_num_to_node[node]->free_tree) {
        size += p.first * p.second.size();
    }
    return size;
}

int AllocatorTest::GetTreeSize(int node) {
    return aloc.numa_num_to_node[node]->free_tree.size();
}

void AllocatorTest::AllocateZeroTest() {
    EXPECT_TRUE(mmap_alloc(0_page, 0) == nullptr);
}

void AllocatorTest::HashSegTest() {
    void* ptr = mmap_alloc(PageSizeToBytes(aloc.kHashSegPageSize), 0);
    EXPECT_TRUE(ptr != nullptr);
    EXPECT_EQ(1, aloc.occupied_hash_segs.size());
    EXPECT_EQ(0, aloc.reserved_hash_segs.size());
    mmap_free(ptr);
    EXPECT_EQ(0, aloc.occupied_hash_segs.size());
    EXPECT_EQ(1, aloc.reserved_hash_segs.size());
    ptr = mmap_alloc(PageSizeToBytes(aloc.kHashSegPageSize), 0);
    EXPECT_TRUE(ptr != nullptr);
    EXPECT_EQ(0, aloc.reserved_hash_segs.size());
    mmap_free(ptr);
}

void AllocatorTest::BstFreePagesTest() {
    int tot_size = INIT_HEAP_PAGE_SIZE * aloc.numa_num_to_node.size();
    EXPECT_EQ(tot_size, aloc.FreePages());
    void* ptr = mmap_alloc(PageSizeToBytes(aloc.kHashSegPageSize), 0);
    EXPECT_EQ(tot_size, aloc.FreePages());
    mmap_free(ptr);
    ptr = mmap_alloc(PageSizeToBytes(10), 0);
    EXPECT_EQ(tot_size - 10, aloc.FreePages());
    mmap_free(ptr);
    EXPECT_EQ(tot_size, aloc.FreePages());
}

void AllocatorTest::BstChangeProtTest() {
    void* ptr = mmap_alloc(PageSizeToBytes(1), 0);
    mmap_prot_read(ptr);
    char c = *(reinterpret_cast<char*>(ptr));
    mmap_prot_readwrite(ptr);
    int* pInt = reinterpret_cast<int*>(ptr);
    pInt[0] = 1;
    mmap_free(ptr);
}

void AllocatorTest::BstChunkPointerTest() {
    vector<void*> ptrs;
    int num_ptrs = aloc.ptr_to_bstchunk.size();
    for (int i = 1; i <= 3; i++) {
        ptrs.push_back(mmap_alloc(PageSizeToBytes(i * 10), 0));
        EXPECT_TRUE(ptrs.back() != nullptr);
        EXPECT_EQ(num_ptrs + i, aloc.ptr_to_bstchunk.size());
    }
    EXPECT_EQ(kBSTHeapSize - aloc.AllocatedPages(), GetFreeBSTSize(0));
    // check sibling list
    for (const auto p : aloc.ptr_to_bstchunk) {
        if (p.second->next)
            EXPECT_EQ(PageSizeToBytes(p.second->size), subtract(p.second->next->mem_ptr, p.second->mem_ptr));
        if (p.second->prev)
            EXPECT_EQ(PageSizeToBytes(p.second->prev->size), subtract(p.second->mem_ptr, p.second->prev->mem_ptr));
    }
    for (auto p : ptrs) {
        mmap_free(p);
    }
    EXPECT_EQ(1, GetTreeSize(0));
    EXPECT_EQ(num_ptrs, aloc.ptr_to_bstchunk.size());
    EXPECT_EQ(0, GetAllocatedBSTSize());
    EXPECT_EQ(kBSTHeapSize, GetFreeBSTSize(0));
}

void AllocatorTest::BstCoalesceTest() {
    vector<void*> ptrs;
    int num_ptrs = aloc.ptr_to_bstchunk.size();
    for (int i = 1; i <= 3; i++) {
        ptrs.push_back(mmap_alloc(PageSizeToBytes(i * 10), 0));
        EXPECT_TRUE(ptrs.back() != nullptr);
        EXPECT_EQ(num_ptrs + i, aloc.ptr_to_bstchunk.size());
    }
    mmap_free(ptrs[1]);
    EXPECT_EQ(num_ptrs + 3, aloc.ptr_to_bstchunk.size());
    mmap_free(ptrs[2]);
    EXPECT_EQ(num_ptrs + 1, aloc.ptr_to_bstchunk.size());
    mmap_free(ptrs[0]);
    EXPECT_EQ(num_ptrs, aloc.ptr_to_bstchunk.size());
    EXPECT_EQ(0, GetAllocatedBSTSize());
    EXPECT_EQ(kBSTHeapSize, GetFreeBSTSize(0));
    EXPECT_EQ(1, GetTreeSize(0));
}

void AllocatorTest::BstFixedSizeTest() {
    for (int size = 1; size < kBSTHeapSize; size <<= 1) {
        vector<void*> ptrs;
        for (int i = 1; i < kBSTHeapSize / size; i++) {
            ptrs.push_back(mmap_alloc(PageSizeToBytes(size), 0));
            EXPECT_TRUE(ptrs.back() != nullptr);
            EXPECT_EQ(i * size, aloc.AllocatedPages());
            EXPECT_EQ(aloc.AllocatedPages(), GetAllocatedBSTSize());
        }
        for (auto p : ptrs) {
            mmap_free(p);
        }
        EXPECT_EQ(0, GetAllocatedBSTSize());
        EXPECT_EQ(kBSTHeapSize, GetFreeBSTSize(0));
        EXPECT_EQ(1, GetTreeSize(0));
    }
}

void AllocatorTest::BstSmallToLargeTest() {
    vector<void*> ptrs;
    int num_ptrs = aloc.ptr_to_bstchunk.size();
    for (int size = 1; size < kBSTHeapSize; size <<= 1) {
        ptrs.push_back(mmap_alloc(PageSizeToBytes(size), 0));
        EXPECT_TRUE(ptrs.back() != nullptr);
        EXPECT_EQ(++num_ptrs, aloc.ptr_to_bstchunk.size());
    }
    EXPECT_EQ(kBSTHeapSize - 1, GetAllocatedBSTSize());
    EXPECT_EQ(1, GetFreeBSTSize(0));
    for (auto p : ptrs) {
        mmap_free(p);
    }
    EXPECT_EQ(0, GetAllocatedBSTSize());
    EXPECT_EQ(kBSTHeapSize, GetFreeBSTSize(0));
    EXPECT_EQ(1, GetTreeSize(0));
}

void AllocatorTest::BstLargeToSmallTest() {
    vector<void*> ptrs;
    int num_ptrs = aloc.ptr_to_bstchunk.size();
    for (int size = kBSTHeapSize >> 1; size >= 1; size >>= 1) {
        ptrs.push_back(mmap_alloc(PageSizeToBytes(size), 0));
        EXPECT_TRUE(ptrs.back() != nullptr);
        EXPECT_EQ(++num_ptrs, aloc.ptr_to_bstchunk.size());
    }
    EXPECT_EQ(kBSTHeapSize - 1, GetAllocatedBSTSize());
    EXPECT_EQ(1, GetFreeBSTSize(0));
    for (auto p : ptrs) {
        mmap_free(p);
    }
    EXPECT_EQ(0, GetAllocatedBSTSize());
    EXPECT_EQ(kBSTHeapSize, GetFreeBSTSize(0));
    EXPECT_EQ(1, GetTreeSize(0));
}

void AllocatorTest::BstVariousFillTest() {
    vector<void*> ptrs;
    int tot_size = 0;
    for (int p_size : {7, 14, 2, 5, 14, 50, 15, 3, 127, 32, 1, 8}) {
        ptrs.push_back(mmap_alloc(PageSizeToBytes(p_size), 0));
        EXPECT_TRUE(ptrs.back() != nullptr);
        tot_size += p_size;
        EXPECT_EQ(tot_size, GetAllocatedBSTSize());
    }
    for (auto p : ptrs) {
        mmap_free(p);
    }
    EXPECT_EQ(0, GetAllocatedBSTSize());
    EXPECT_EQ(kBSTHeapSize, GetFreeBSTSize(0));
    EXPECT_EQ(1, GetTreeSize(0));
}

void AllocatorTest::BstFreeHalfRefillTest1() {
    vector<void*> ptrs;
    vector<int> sizes{7, 14, 2, 50, 15, 127, 32, 1};
    int tot_size = 0;
    for (int p_size : sizes) {
        ptrs.push_back(mmap_alloc(PageSizeToBytes(p_size), 0));
        EXPECT_TRUE(ptrs.back() != nullptr);
        tot_size += p_size;
        EXPECT_EQ(tot_size, GetAllocatedBSTSize());
    }
    for (int i = 0; i < ptrs.size(); i++) {
        if (i % 2) {
            mmap_free(ptrs[i]);
            ptrs[i] = nullptr;
            tot_size -= sizes[i];
            EXPECT_EQ(tot_size, GetAllocatedBSTSize());
        }
    }
    for (int p_size : {2, 31, 4, 7}) {
        ptrs.push_back(mmap_alloc(PageSizeToBytes(p_size), 0));
        EXPECT_TRUE(ptrs.back() != nullptr);
        tot_size += p_size;
        EXPECT_EQ(tot_size, GetAllocatedBSTSize());
    }
    for (auto p : ptrs) {
        if (p) {
            mmap_free(p);
        }
    }
    EXPECT_EQ(0, GetAllocatedBSTSize());
    EXPECT_EQ(kBSTHeapSize, GetFreeBSTSize(0));
    EXPECT_EQ(1, GetTreeSize(0));
}

void AllocatorTest::BstFreeHalfRefillTest2() {
    vector<void*> ptrs;
    vector<int> sizes{7, 14, 2, 50, 15, 127, 32, 1};
    int tot_size = 0;
    for (int p_size : sizes) {
        ptrs.push_back(mmap_alloc(PageSizeToBytes(p_size), 0));
        EXPECT_TRUE(ptrs.back() != nullptr);
        tot_size += p_size;
        EXPECT_EQ(tot_size, GetAllocatedBSTSize());
    }
    for (int i = 0; i < ptrs.size(); i++) {
        if (!(i % 2)) {
            mmap_free(ptrs[i]);
            ptrs[i] = nullptr;
            tot_size -= sizes[i];
            EXPECT_EQ(tot_size, GetAllocatedBSTSize());
        }
    }
    for (int p_size : {2, 31, 4, 7}) {
        ptrs.push_back(mmap_alloc(PageSizeToBytes(p_size), 0));
        EXPECT_TRUE(ptrs.back() != nullptr);
        tot_size += p_size;
        EXPECT_EQ(tot_size, GetAllocatedBSTSize());
    }
    for (auto p : ptrs) {
        if (p) {
            mmap_free(p);
        }
    }
    EXPECT_EQ(0, GetAllocatedBSTSize());
    EXPECT_EQ(kBSTHeapSize, GetFreeBSTSize(0));
    EXPECT_EQ(1, GetTreeSize(0));
}

void AllocatorTest::BstFreeOneThirdRefillTest() {
    vector<void*> ptrs;
    vector<int> sizes{7, 14, 2, 50, 15, 127, 32, 1, 9};
    int tot_size = 0;
    for (int p_size : sizes) {
        ptrs.push_back(mmap_alloc(PageSizeToBytes(p_size), 0));
        EXPECT_TRUE(ptrs.back() != nullptr);
        tot_size += p_size;
        EXPECT_EQ(tot_size, GetAllocatedBSTSize());
    }
    for (int i = 0; i < ptrs.size(); i++) {
        if (i % 3) {
            mmap_free(ptrs[i]);
            ptrs[i] = nullptr;
            tot_size -= sizes[i];
            EXPECT_EQ(tot_size, GetAllocatedBSTSize());
        }
    }
    for (int p_size : {2, 31, 4, 7, 75}) {
        ptrs.push_back(mmap_alloc(PageSizeToBytes(p_size), 0));
        EXPECT_TRUE(ptrs.back() != nullptr);
        tot_size += p_size;
        EXPECT_EQ(tot_size, GetAllocatedBSTSize());
    }
    for (auto p : ptrs) {
        if (p) {
            mmap_free(p);
        }
    }
    EXPECT_EQ(0, GetAllocatedBSTSize());
    EXPECT_EQ(kBSTHeapSize, GetFreeBSTSize(0));
    EXPECT_EQ(1, GetTreeSize(0));
}

void AllocatorTest::MultiNodeAllocateTest() {
    vector<void*> ptrs;
    ptrs.push_back(mmap_alloc(PageSizeToBytes(kBSTHeapSize), 0));
    EXPECT_TRUE(ptrs.back() != nullptr);
    EXPECT_EQ(0, GetTreeSize(0));
    ptrs.push_back(mmap_alloc(PageSizeToBytes(kBSTHeapSize), 0));
    EXPECT_TRUE(ptrs.back() != nullptr);
    EXPECT_EQ(0, GetTreeSize(1));
    ptrs.push_back(mmap_alloc(PageSizeToBytes(kBSTHeapSize), 1));
    EXPECT_TRUE(ptrs.back() != nullptr);
    EXPECT_EQ(0, GetTreeSize(2));
    for (auto p : ptrs) {
        mmap_free(p);
    }
    EXPECT_EQ(0, GetAllocatedBSTSize());
}

void AllocatorTest::MultiNodeFreeTest() {
    void* ptr1 = mmap_alloc(PageSizeToBytes(kBSTHeapSize), 0);
    EXPECT_TRUE(ptr1 != nullptr);
    EXPECT_EQ(0, GetTreeSize(0));
    void* ptr2 = mmap_alloc(PageSizeToBytes(kBSTHeapSize), 0);
    EXPECT_TRUE(ptr2 != nullptr);
    EXPECT_EQ(0, GetTreeSize(1));
    void* ptr3 = mmap_alloc(PageSizeToBytes(kBSTHeapSize), 1);
    EXPECT_TRUE(ptr3 != nullptr);
    EXPECT_EQ(0, GetTreeSize(2));
    mmap_free(ptr2);
    EXPECT_EQ(1, GetTreeSize(1));
    mmap_free(ptr3);
    EXPECT_EQ(1, GetTreeSize(2));
    mmap_free(ptr1);
    for (int i : {0, 1, 2}) {
        EXPECT_EQ(kBSTHeapSize, GetFreeBSTSize(i));
    }
    EXPECT_EQ(0, GetAllocatedBSTSize());
}

#ifdef TEST_NUMA_LOGIC
void AllocatorTest::BstHeapGrowTest() {
    vector<void*> ptrs;
    for (int i = 0; i < aloc.numa_num_to_node.size(); i++) {
        ptrs.push_back(mmap_alloc(PageSizeToBytes(kBSTHeapSize), i));
        EXPECT_TRUE(ptrs.back() != nullptr);
    }
    ptrs.push_back(mmap_alloc(PageSizeToBytes(kBSTHeapSize), 0));
    for (auto p : ptrs) {
        mmap_free(p);
    }
    EXPECT_EQ(0, GetAllocatedBSTSize());
    EXPECT_EQ(HEAP_GROW_BY_SIZE + kBSTHeapSize, GetFreeBSTSize(0));
}
#else
void AllocatorTest::BstHeapGrowTest() {
    void* ptr1 = mmap_alloc(PageSizeToBytes(kBSTHeapSize), 0);
    EXPECT_TRUE(ptr1 != nullptr);
    void* ptr2 = mmap_alloc(PageSizeToBytes(kBSTHeapSize), 0);
    EXPECT_TRUE(ptr2 != nullptr);
    EXPECT_EQ(HEAP_GROW_BY_SIZE - kBSTHeapSize, GetFreeBSTSize(0));
    mmap_free(ptr1);
    mmap_free(ptr2);
    EXPECT_EQ(0, GetAllocatedBSTSize());
    EXPECT_EQ(HEAP_GROW_BY_SIZE + kBSTHeapSize, GetFreeBSTSize(0));
}
#endif

TEST_F(AllocatorTest, AllocateZeroTest) { AllocateZeroTest(); }
TEST_F(AllocatorTest, HashSegTest) { HashSegTest(); }
TEST_F(AllocatorTest, BstFreePagesTest) { BstFreePagesTest(); }
TEST_F(AllocatorTest, BstChangeProtTest) { BstChangeProtTest(); }
TEST_F(AllocatorTest, BstChunkPointerTest) { BstChunkPointerTest(); }
TEST_F(AllocatorTest, BstCoalesceTest) { BstCoalesceTest(); }
TEST_F(AllocatorTest, BstFixedSizeTest) { BstFixedSizeTest(); }
TEST_F(AllocatorTest, BstSmallToLargeTest) { BstSmallToLargeTest(); }
TEST_F(AllocatorTest, BstLargeToSmallTest) { BstLargeToSmallTest(); }
TEST_F(AllocatorTest, BstVariousFillTest) { BstVariousFillTest(); }
TEST_F(AllocatorTest, BstFreeHalfRefillTest1) { BstFreeHalfRefillTest1(); }
TEST_F(AllocatorTest, BstFreeHalfRefillTest2) { BstFreeHalfRefillTest1(); }
TEST_F(AllocatorTest, BstFreeOneThirdRefillTest) { BstFreeOneThirdRefillTest(); }
#ifdef TEST_NUMA_LOGIC
TEST_F(AllocatorTest, MultiNodeAllocateTest) { MultiNodeAllocateTest(); }
TEST_F(AllocatorTest, MultiNodeFreeTest) { MultiNodeFreeTest(); }
#endif
TEST_F(AllocatorTest, BstHeapGrowTest) { BstHeapGrowTest(); }

int main(int argc, char **argv) {
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
