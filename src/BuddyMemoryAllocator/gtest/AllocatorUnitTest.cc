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

    // virtual void SetUp() {
        // Code here will be called immediately after the constructor (right
        // before each test).
    // }

    // virtual void TearDown() {
        // Code here will be called immediately after each test (right
        // before the destructor).
    // }

    // write the tests as members of the fixture class to access private members,
    // since even though this test fixture is a friend to allocator class, the
    // tests(sub-classes of the fixture) are not automatically friends to it
    void AllocateNonPositiveTest();
    void GetBuddyOrderTest();
    void BuddyAllocateTest();
    void BuddyFreeTest();
    void BstAllocateTest();
    void BstFreeTest();
    void HashSegTest();

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

void AllocatorTest::AllocateNonPositiveTest() {
    // EXPECT_TRUE(aloc.MmapAlloc(0_page, node, filename, linenum) == nullptr);
    // EXPECT_TRUE(aloc.MmapAlloc(-1, node, filename, linenum) == nullptr);
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

void AllocatorTest::BuddyAllocateTest() {
    EXPECT_TRUE(aloc.MmapAlloc(1_page, node, filename, linenum) != nullptr);
    for (int i = 0; i < MAX_ORDER; i++) {
        EXPECT_EQ(1, aloc.free_area[i].size());
    }
    EXPECT_EQ(1, aloc.AllocatedPages());
    EXPECT_EQ(MAX_ORDER + 1, aloc.ptr_to_budchunk.size());
    EXPECT_TRUE(aloc.MmapAlloc(7_page, node, filename, linenum) != nullptr);
    EXPECT_EQ(0, aloc.free_area[3].size());
    EXPECT_EQ(9, aloc.AllocatedPages());
    EXPECT_TRUE(aloc.MmapAlloc(5_page, node, filename, linenum) != nullptr);
    EXPECT_EQ(17, aloc.AllocatedPages());
    EXPECT_EQ(aloc.AllocatedPages(), GetAllocatedBuddySize());
    EXPECT_EQ(kBuddyHeapSize - aloc.AllocatedPages() , GetFreeBuddySize());
}

void AllocatorTest::BuddyFreeTest() {
    void* ptr1 = aloc.MmapAlloc(1_page, node, filename, linenum);
    EXPECT_TRUE(ptr1 != nullptr);
    aloc.MmapFree(ptr1);
    EXPECT_EQ(1, aloc.ptr_to_budchunk.size());
    for (int i = 0; i < MAX_ORDER; i++) {
        EXPECT_EQ(0, aloc.free_area[i].size());
    }
    EXPECT_EQ(1, aloc.free_area[MAX_ORDER].size());
    EXPECT_EQ(5, aloc.budchunk_pool.size());
    ptr1 = aloc.MmapAlloc(1_page, node, filename, linenum);
    void* ptr2 = aloc.MmapAlloc(1_page, node, filename, linenum);
    aloc.MmapFree(ptr1);
    for (int i = 0; i < MAX_ORDER; i++) {
        EXPECT_EQ(1, aloc.free_area[i].size());
    }
    EXPECT_EQ(aloc.AllocatedPages(), GetAllocatedBuddySize());
    EXPECT_EQ(kBuddyHeapSize - aloc.AllocatedPages(), GetFreeBuddySize());
}

void AllocatorTest::BstAllocateTest() {
    EXPECT_TRUE(aloc.MmapAlloc(PageToBytes(aloc.kBuddyHeapSize + 1), node, filename, linenum) != nullptr);
    EXPECT_EQ(2, aloc.ptr_to_bstchunk.size());
    EXPECT_TRUE(aloc.MmapAlloc(PageToBytes(aloc.kBuddyHeapSize + 10), node, filename, linenum) != nullptr);
    EXPECT_EQ(3, aloc.ptr_to_bstchunk.size());
    EXPECT_TRUE(aloc.MmapAlloc(PageToBytes(aloc.kBuddyHeapSize + 20), node, filename, linenum) != nullptr);
    EXPECT_EQ(4, aloc.ptr_to_bstchunk.size());
    for (const auto p : aloc.ptr_to_bstchunk) {
        if (p.second->next)
            EXPECT_EQ(PageToBytes(p.second->size), subtract(p.second->next->mem_ptr, p.second->mem_ptr));
        if (p.second->prev)
            EXPECT_EQ(PageToBytes(p.second->prev->size), subtract(p.second->mem_ptr, p.second->prev->mem_ptr));
    }
    EXPECT_EQ(aloc.AllocatedPages(), GetAllocatedBSTSize());
    EXPECT_EQ(kBSTHeapSize - aloc.AllocatedPages(), GetFreeBSTSize());
}

void AllocatorTest::BstFreeTest() {
    void* ptr1 = aloc.MmapAlloc(PageToBytes(aloc.kBuddyHeapSize + 10), node, filename, linenum);
    EXPECT_TRUE(ptr1 != nullptr);
    void* ptr2 = aloc.MmapAlloc(PageToBytes(aloc.kBuddyHeapSize + 20), node, filename, linenum);
    EXPECT_TRUE(ptr2 != nullptr);
    void* ptr3 = aloc.MmapAlloc(PageToBytes(aloc.kBuddyHeapSize + 30), node, filename, linenum);
    EXPECT_TRUE(ptr3 != nullptr);
    EXPECT_EQ(4, aloc.ptr_to_bstchunk.size());
    aloc.MmapFree(ptr2);
    EXPECT_EQ(4, aloc.ptr_to_bstchunk.size());
    aloc.MmapFree(ptr3);
    EXPECT_EQ(2, aloc.ptr_to_bstchunk.size());
    aloc.MmapFree(ptr1);
    EXPECT_EQ(1, aloc.ptr_to_bstchunk.size());
    EXPECT_EQ(aloc.AllocatedPages(), GetAllocatedBSTSize());
    EXPECT_EQ(kBSTHeapSize - aloc.AllocatedPages(), GetFreeBSTSize());
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

TEST_F(AllocatorTest, AllocateNonPositiveTest) { AllocateNonPositiveTest(); }
TEST_F(AllocatorTest, GetBuddyOrderTest) { GetBuddyOrderTest(); }
TEST_F(AllocatorTest, BuddyAllocateTest) { BuddyAllocateTest(); }
TEST_F(AllocatorTest, BuddyFreeTest) { BuddyFreeTest(); }
TEST_F(AllocatorTest, BstAllocateTest) { BstAllocateTest(); }
TEST_F(AllocatorTest, BstFreeTest) { BstFreeTest(); }
TEST_F(AllocatorTest, HashSegTest) { HashSegTest(); }

int main(int argc, char **argv) {
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
