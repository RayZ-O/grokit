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
        // Code here will be called immediately after each test (right
        // before the destructor).
    }

    void PrintBuddyPtrMap();

    void PrintBSTPtrMap();

    void PrintFreeArea();

    void PrintFreeTree();
};

void AllocatorTest::PrintBuddyPtrMap() {
    for (auto p : aloc.ptr_to_budchunk) {
        assert(p.second);
        cout << *p.second << endl;
    }
}

void AllocatorTest::PrintBSTPtrMap() {
    for (auto p : aloc.ptr_to_bstchunk) {
        assert(p.second);
        cout << *p.second << endl;
    }
}

void AllocatorTest::PrintFreeArea() {
    for (int i = 0; i < aloc.free_area.size(); i++) {
        cout << "order:" << i << " index:";
        for (auto idx : aloc.free_area[i]) {
            cout << idx << " ";
        }
        cout << endl;
    }
}

void AllocatorTest::PrintFreeTree() {
    for (auto p : aloc.free_tree) {
        assert(p.second);
        cout << aloc.ptr_to_bstchunk[p.second] << endl;
    }

}

TEST_F(AllocatorTest, GetBuddyOrder) {
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

TEST_F(AllocatorTest, BuddyAllocate) {
    EXPECT_TRUE(aloc.MmapAlloc(0_page, node, filename, linenum) == nullptr);
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
}

TEST_F(AllocatorTest, BuddyFree) {
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
}

TEST_F(AllocatorTest, BstAllocate) {
    EXPECT_TRUE(aloc.MmapAlloc(PageToBytes(aloc.kBuddyPageSize + 1), node, filename, linenum) != nullptr);
    EXPECT_EQ(2, aloc.ptr_to_bstchunk.size());
    EXPECT_TRUE(aloc.MmapAlloc(PageToBytes(aloc.kBuddyPageSize + 10), node, filename, linenum) != nullptr);
    EXPECT_EQ(3, aloc.ptr_to_bstchunk.size());
    EXPECT_TRUE(aloc.MmapAlloc(PageToBytes(aloc.kBuddyPageSize + 20), node, filename, linenum) != nullptr);
    EXPECT_EQ(4, aloc.ptr_to_bstchunk.size());
    for (const auto p : aloc.ptr_to_bstchunk) {
        if (p.second->next)
            EXPECT_EQ(PageToBytes(p.second->size), subtract(p.second->next->mem_ptr, p.second->mem_ptr));
        if (p.second->prev)
            EXPECT_EQ(PageToBytes(p.second->prev->size), subtract(p.second->mem_ptr, p.second->prev->mem_ptr));
    }
}

TEST_F(AllocatorTest, BstFree) {

}

TEST_F(AllocatorTest, HashSegTest) {
    void* ptr1 = aloc.MmapAlloc(PageToBytes(aloc.kHashSegPageSize), node, filename, linenum);
    EXPECT_TRUE(ptr1 != nullptr);
    EXPECT_EQ(1, aloc.occupied_hash_segs.size());
    EXPECT_EQ(0, aloc.reserved_hash_segs.size());
    EXPECT_EQ(aloc.kHashSegPageSize, aloc.AllocatedPages());
    aloc.MmapFree(ptr1);
    EXPECT_EQ(0, aloc.occupied_hash_segs.size());
    EXPECT_EQ(1, aloc.reserved_hash_segs.size());
    EXPECT_EQ(0, aloc.AllocatedPages());
}

int main(int argc, char **argv) {
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
