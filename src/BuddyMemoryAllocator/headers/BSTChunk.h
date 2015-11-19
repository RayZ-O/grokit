#ifndef _BST_CHUNK_H_
#define _BST_CHUNK_H_

#define GUNIT_TEST 1

#include <vector>

class BSTreeChunk {
public:
    void* mem_ptr;
    int size;
    bool used;
    BSTreeChunk* prev; // pointer to previous physical chunk
    BSTreeChunk* next; // pointer to next physical chunk

    BSTreeChunk(void* ptr, int s, bool u, BSTreeChunk* p, BSTreeChunk* n);

    BSTreeChunk(const BSTreeChunk& other) = delete;

    BSTreeChunk& operator = (const BSTreeChunk& other) = delete;

    void Assign(void* ptr, int s, bool u, BSTreeChunk* p, BSTreeChunk* n);

    BSTreeChunk* Split(int size);

    std::pair<BSTreeChunk*, bool> CoalescePrev();

    std::pair<BSTreeChunk*, bool> CoalesceNext();

    // erase pointer in the given size set in free tree
    void EraseTreePtr(int size, void* ptr);

    static BSTreeChunk* GetChunk(void* ptr, int s, bool u, BSTreeChunk* p, BSTreeChunk* n);

#ifdef GUNIT_TEST
    friend std::ostream& operator <<(std::ostream &output, BSTreeChunk &chunk);
#endif

private:
    static std::vector<BSTreeChunk*> bstchunk_pool;
};

#endif
