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
    int node;
    char* filename;
    int linenum;

    AllocatorTest() : aloc(BuddyMemoryAllocator::GetAllocator()) {
        node = 0;
        filename = nullptr;
        linenum = 0;
    }

    virtual void SetUp() {

    }

    virtual void TearDown() {
    }

    // write the tests as members of the fixture class to access private members,
    // since even though this test fixture is a friend to allocator class, the
    // tests(sub-classes of the fixture) are not automatically friends to it
    void AllocateZeroTest();
    void HashSegTest();
    // binary tree syetem tests
    void BstAllocateTest();
    void BstFreeTest();
    // TODO more bst tests
    // pretty print the internal data structure infomation
    void PrintBSTPtrMap();
    void PrintFreeTree();
    // use the internal data structure to calculate allocated and free pages
    int GetAllocatedBSTSize();
    int GetFreeBSTSize();
};

constexpr int AllocatorTest::kBSTHeapSize;

void AllocatorTest::PrintBSTPtrMap() {
    cout << "------------Ptr to BST-----------------" << endl;
    for (const auto p : aloc.ptr_to_bstchunk) {
        cout << *p.second << endl;
    }
    cout << "---------------------------------------" << endl;
}

void AllocatorTest::PrintFreeTree() {
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

int AllocatorTest::GetFreeBSTSize() {
    int size = 0;
    for (const auto p : aloc.numa_num_to_node[node]->free_tree) {
        size += p.first * p.second.size();
    }
    return size;
}

void AllocatorTest::AllocateZeroTest() {
    EXPECT_TRUE(mmap_alloc(0_page, node) == nullptr);
}

void AllocatorTest::HashSegTest() {
    void* ptr = mmap_alloc(PageSizeToBytes(aloc.kHashSegPageSize), node);
    EXPECT_TRUE(ptr != nullptr);
    EXPECT_EQ(1, aloc.occupied_hash_segs.size());
    EXPECT_EQ(0, aloc.reserved_hash_segs.size());
    mmap_free(ptr);
    EXPECT_EQ(0, aloc.occupied_hash_segs.size());
    EXPECT_EQ(1, aloc.reserved_hash_segs.size());
    ptr = mmap_alloc(PageSizeToBytes(aloc.kHashSegPageSize), node);
    EXPECT_TRUE(ptr != nullptr);
    EXPECT_EQ(0, aloc.reserved_hash_segs.size());
    mmap_free(ptr);
}
void AllocatorTest::BstAllocateTest() {
    vector<void*> ptrs;
    for (int i = 1; i <= 3; i++) {
        ptrs.push_back(mmap_alloc(PageSizeToBytes(i * 10), node));
        EXPECT_TRUE(ptrs.back() != nullptr);
        EXPECT_EQ(i + 1, aloc.ptr_to_bstchunk.size());
    }

    EXPECT_EQ(aloc.AllocatedPages(), GetAllocatedBSTSize());
    EXPECT_EQ(kBSTHeapSize - aloc.AllocatedPages(), GetFreeBSTSize());
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
    EXPECT_EQ(1, aloc.numa_num_to_node[node]->free_tree.size());
    EXPECT_EQ(1, aloc.ptr_to_bstchunk.size());
    EXPECT_EQ(0, GetAllocatedBSTSize());
    EXPECT_EQ(kBSTHeapSize, GetFreeBSTSize());
}

void AllocatorTest::BstFreeTest() {
    vector<void*> ptrs;
    for (int i = 1; i <= 3; i++) {
        ptrs.push_back(mmap_alloc(PageSizeToBytes(i * 10), node));
        EXPECT_TRUE(ptrs.back() != nullptr);
        EXPECT_EQ(i + 1, aloc.ptr_to_bstchunk.size());
    }
    mmap_free(ptrs[1]);
    EXPECT_EQ(4, aloc.ptr_to_bstchunk.size());
    mmap_free(ptrs[2]);
    EXPECT_EQ(2, aloc.ptr_to_bstchunk.size());
    mmap_free(ptrs[0]);
    EXPECT_EQ(1, aloc.ptr_to_bstchunk.size());
    EXPECT_EQ(0, GetAllocatedBSTSize());
    EXPECT_EQ(kBSTHeapSize, GetFreeBSTSize());
}

TEST_F(AllocatorTest, AllocateZeroTest) { AllocateZeroTest(); }
TEST_F(AllocatorTest, HashSegTest) { HashSegTest(); }
TEST_F(AllocatorTest, BstAllocateTest) { BstAllocateTest(); }
TEST_F(AllocatorTest, BstFreeTest) { BstFreeTest(); }

int main(int argc, char **argv) {
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
