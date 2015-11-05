#include <gtest/gtest.h>
#include "../headers/BuddyMemoryAllocator.h"

using namespace std;

constexpr size_t operator "" _page(unsigned long long page_size) {
    return page_size << ALLOC_PAGE_SIZE_EXPONENT;
}

class AllocatorTest : public ::testing::Test {
protected:
    AllocatorTest() : aloc(BuddyMemoryAllocator::GetAllocator()) {
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

    BuddyMemoryAllocator& aloc;
    int node;
    char* filename;
    int linenum;
};

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
    EXPECT_EQ(1, aloc.AllocatedPages());
    EXPECT_EQ(MAX_ORDER + 1, aloc.ptr_to_budchunk.size());
    EXPECT_TRUE(aloc.MmapAlloc(7_page, node, filename, linenum) != nullptr);
    EXPECT_TRUE(aloc.MmapAlloc(5_page, node, filename, linenum) != nullptr);
}

TEST_F(AllocatorTest, BstAllocate) {
    EXPECT_TRUE(aloc.MmapAlloc(aloc.kBuddyPageSize + 1, node, filename, linenum) != nullptr);
}

TEST_F(AllocatorTest, HashSegAllocate) {
    EXPECT_TRUE(aloc.MmapAlloc(aloc.kHashSegPageSize, node, filename, linenum) != nullptr);
    EXPECT_EQ(aloc.kHashSegPageSize, aloc.AllocatedPages());
    EXPECT_EQ(1, aloc.occupied_hash_segs.size());
    EXPECT_EQ(0, aloc.reserved_hash_segs.size());
}

int main(int argc, char **argv) {
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
