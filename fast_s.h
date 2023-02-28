/*
   Copyright (c) 2018, UNIST. All rights reserved. The license is a free
   non-exclusive, non-transferable license to reproduce, use, modify and display
   the source code version of the Software, with or without modifications solely
   for non-commercial research, educational or evaluation purposes. The license
   does not entitle Licensee to technical support, telephone assistance,
   enhancements or updates to the Software. All rights, title to and ownership
   interest in the Software, including all intellectual property rights therein
   shall remain in UNIST.

   Please use at your own risk.
*/

#include <cassert>
#include <climits>
#include <fstream>
#include <future>
#include <iostream>
#include <libpmemobj.h>
#include <math.h>
#include <mutex>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <vector>
#include <immintrin.h>
// #include <stdatomic.h>
#include "stallocator_dis.h"
#include "nvm_malloc.h"
#include <omp.h>
#define INNER_PAGESIZE (512)
#define LEAF_PAGESIZE (256)
// #define STM
// #define PMDK
// #define NV_MALLOC
#define CACHE_LINE_SIZE 64

#define IS_FORWARD(c) (c % 2 == 0)

class btree;
class inner_page;
class leaf_page;

POBJ_LAYOUT_BEGIN(btree);
POBJ_LAYOUT_ROOT(btree, btree);
POBJ_LAYOUT_TOID(btree, inner_page);
POBJ_LAYOUT_TOID(btree, leaf_page);
POBJ_LAYOUT_END(btree);

using entry_key_t = uint64_t;
PMEMobjpool *pool;
pm_allocator *alloc;
pthread_mutex_t print_mtx;
int range_node_cnt = 0;
uint64_t split_cnt = 1;
uint64_t delete_cnt = 0;
uint64_t fail_cnt;
void *base_addr;
using namespace std;
uint64_t time_cnt[64];

long long time_m;
struct timespec start_timer, end_timer;
int get_rand()
{
  // srand(time(0));
  int ret = rand() % 500;
  // printf("the rand is %lu\n", ret);
  return 0;
}
static inline void persist_page(void *data, int len)
{
  uintptr_t uptr;
  for (uptr = (uintptr_t)data & ~(63);
       uptr < (uintptr_t)data + len; uptr += 64)
    asm volatile(".byte 0x66; xsaveopt %0"
                 : "+m"(*(volatile char *)uptr));
  asm volatile("sfence" ::
                   : "memory");
}

static inline void persist(void *data, int length = 1)
{
  asm volatile(".byte 0x66; xsaveopt %0"
               : "+m"(*(volatile char *)data));
  asm volatile("sfence" ::
                   : "memory");
}

class btree
{
public:
  inner_page *
      root;
  PMEMobjpool *pop;
  void *base;
  uint64_t leftbottom;
  int height;

public:
  btree();
  void constructor();
  void setNewRoot(inner_page *root);
  void btree_insert(entry_key_t, char *, int);
  void btree_insert_internal(char *, entry_key_t, char *, uint32_t);
  char *btree_search(entry_key_t);
  leaf_page *btree_node_search(entry_key_t, uint64_t, bool);
  void btree_search_range(entry_key_t, entry_key_t, unsigned long *, int);
  void btree_delete(entry_key_t, int);
  void btree_delete_internal(entry_key_t, char *, uint32_t, entry_key_t *,
                             bool *, inner_page **);
  void btree_update(entry_key_t, char *);
  void printAll();
  void recover(int);
  void check();
  void randScounter();

  friend class leaf_page;
  friend class inner_page;
};

class inner_header
{
private:
  uint64_t highest; // 8 bytes
  uint64_t lowest;
  inner_page *leftmost_ptr; // 8 bytes
  inner_page *sibling_ptr;  // 8 bytes
  uint32_t level;           // 4 bytes
  uint8_t switch_counter;   // 1 bytes
  uint8_t is_deleted;       // 1 bytes
  int16_t last_index;       // 2 bytes
  std::mutex *mtx;          // 8 bytes

  friend class inner_page;
  friend class btree;

public:
  inner_header()
  {
    mtx = new std::mutex();
    leftmost_ptr = NULL;
    sibling_ptr = NULL;
    switch_counter = 0;
    last_index = -1;
    is_deleted = false;
    lowest = 0;
  }

  ~inner_header() { delete mtx; }
};

class leaf_header
{
public:
  meta_template meta;       // 8 bytes
  uint64_t lowest;          // 8 bytes
  inner_page *leftmost_ptr; // 8 bytes
  uint64_t sibling_ptr;     // 8 bytes

  uint32_t level;         // 4 bytes
  uint8_t switch_counter; // 1 bytes
  uint8_t is_deleted;     // 1 bytes
  int16_t last_index;     // 2 bytes
  std::mutex *mtx;        // 8 bytes

  friend class leaf_page;
  friend class btree;

public:
  void constructor()
  {
    mtx = new std::mutex();

    leftmost_ptr = NULL;
    sibling_ptr = 0;
    switch_counter = 0;
    last_index = -1;
    is_deleted = false;
    lowest = 0;
#ifdef STM
    memset(&meta, 0, 8);
    // printf("valid is %d, usage is %d, referenced is %d\n", meta.valid, meta.usage, meta.referenced);
#endif
  }

  ~leaf_header()
  {
    delete mtx;
  }
};

class entry
{
private:
  entry_key_t key; // 8 bytes
  char *ptr;       // 8 bytes

public:
  void constructor()
  {
    key = LONG_MAX;
    ptr = NULL;
  }

  friend class inner_page;
  friend class leaf_page;
  friend class btree;
};

const int leaf_cardinality = (LEAF_PAGESIZE - sizeof(leaf_header)) / sizeof(entry);
const int inner_cardinality = (INNER_PAGESIZE - sizeof(inner_header)) / sizeof(entry);
const int count_in_line = CACHE_LINE_SIZE / sizeof(entry);

class inner_page
{
private:
  inner_header hdr;                 // header in persistent memory, 16 bytes
  entry records[inner_cardinality]; // slots in persistent memory, 16 bytes * n

public:
  friend class btree;

  inner_page(uint32_t level = 0)
  {
    hdr.level = level;
    records[0].ptr = NULL;
  }

  // this is called when tree grows
  inner_page(inner_page *left, entry_key_t key, inner_page *right, uint32_t level = 0)
  {
    hdr.leftmost_ptr = left;
    hdr.level = level;
    records[0].key = key;
    records[0].ptr = (char *)right;
    records[1].ptr = NULL;

    hdr.last_index = 0;
  }

  void *operator new(size_t size)
  {
    void *ret;
    posix_memalign(&ret, 64, size);
    return ret;
  }

  inline int count()
  {
    uint8_t previous_switch_counter;
    int count = 0;
    do
    {
      previous_switch_counter = hdr.switch_counter;
      count = hdr.last_index + 1;

      while (count >= 0 && records[count].ptr != NULL)
      {
        if (IS_FORWARD(previous_switch_counter))
          ++count;
        else
          --count;
      }

      if (count < 0)
      {
        count = 0;
        while (records[count].ptr != NULL)
        {
          ++count;
        }
      }

    } while (previous_switch_counter != hdr.switch_counter);

    return count;
  }

  inline void insert_key(entry_key_t key, char *ptr, int *num_entries,
                         bool flush = true, bool update_last_index = true)
  {
    // update switch_counter
    if (!IS_FORWARD(hdr.switch_counter))
      ++hdr.switch_counter;

    // FAST
    if (*num_entries == 0)
    { // this page is empty
      entry *new_entry = (entry *)&records[0];
      entry *array_end = (entry *)&records[1];
      new_entry->key = (entry_key_t)key;
      new_entry->ptr = (char *)ptr;

      array_end->ptr = (char *)NULL;
    }
    else
    {
      int i = *num_entries - 1, inserted = 0, to_flush_cnt = 0;
      records[*num_entries + 1].ptr = records[*num_entries].ptr;

      // FAST
      for (i = *num_entries - 1; i >= 0; i--)
      {
        if (key < records[i].key)
        {
          records[i + 1].ptr = records[i].ptr;
          records[i + 1].key = records[i].key;
        }
        else
        {
          records[i + 1].ptr = records[i].ptr;
          records[i + 1].key = key;
          records[i + 1].ptr = ptr;
          inserted = 1;
          break;
        }
      }
      if (inserted == 0)
      {
        records[0].ptr = (char *)hdr.leftmost_ptr;
        records[0].key = key;
        records[0].ptr = ptr;
      }
    }

    if (update_last_index)
    {
      hdr.last_index = *num_entries;
    }
    ++(*num_entries);
  }

  // Insert a new key - FAST and FAIR
  inner_page *store(btree *bt, char *left, entry_key_t key, char *right, bool flush,
                    bool with_lock, inner_page *invalid_sibling = NULL)
  {
    if (with_lock)
    {
      hdr.mtx->lock(); // Lock the write lock
    }
    if (hdr.is_deleted)
    {
      if (with_lock)
      {
        hdr.mtx->unlock();
      }

      return NULL;
    }

    // If this node has a sibling node,
    if (hdr.sibling_ptr && (hdr.sibling_ptr != invalid_sibling))
    {
      // Compare this key with the first key of the sibling
      if (key > hdr.sibling_ptr->hdr.highest)
      {
        if (with_lock)
        {
          hdr.mtx->unlock(); // Unlock the write lock
        }
        return hdr.sibling_ptr->store(bt, NULL, key, right, true, with_lock,
                                      invalid_sibling);
      }
    }

    register int num_entries = count();

    // FAST
    if (num_entries < inner_cardinality - 1)
    {
      insert_key(key, right, &num_entries, flush);

      if (with_lock)
      {
        hdr.mtx->unlock(); // Unlock the write lock
      }

      return this;
    }
    else
    { // FAIR
      // overflow
      // create a new node
      inner_page *sibling = new inner_page(hdr.level);
      register int m = (int)ceil(num_entries / 2);
      entry_key_t split_key = records[m].key;

      // migrate half of keys into the sibling
      int sibling_cnt = 0;
      // internal node
      for (int i = m + 1; i < num_entries; ++i)
      {
        sibling->insert_key(records[i].key, records[i].ptr, &sibling_cnt,
                            false);
      }
      sibling->hdr.highest = records[m].key;
      sibling->hdr.leftmost_ptr = (inner_page *)records[m].ptr;
      sibling->hdr.sibling_ptr = hdr.sibling_ptr;
      sibling->hdr.lowest = split_key;
      hdr.sibling_ptr = sibling;
      hdr.highest = split_key;
      // set to NULL
      if (IS_FORWARD(hdr.switch_counter))
        hdr.switch_counter += 2;
      else
        ++hdr.switch_counter;
      records[m].ptr = NULL;

      hdr.last_index = m - 1;

      num_entries = hdr.last_index + 1;

      inner_page *ret;

      // insert the key
      if (key < split_key)
      {
        insert_key(key, right, &num_entries);
        ret = this;
      }
      else
      {
        sibling->insert_key(key, right, &sibling_cnt);
        ret = sibling;
      }

      // Set a new root or insert the split key to the parent
      if (bt->root == this)
      { // only one node can update the root ptr
        inner_page *new_root =
            new inner_page((inner_page *)this, split_key, sibling, hdr.level + 1);
        bt->setNewRoot(new_root);

        if (with_lock)
        {
          hdr.mtx->unlock(); // Unlock the write lock
        }
      }
      else
      {
        if (with_lock)
        {
          hdr.mtx->unlock(); // Unlock the write lock
        }
        bt->btree_insert_internal(NULL, split_key, (char *)sibling,
                                  hdr.level + 1);
      }

      return ret;
    }
  }

  char *linear_search(entry_key_t key, inner_page *node = NULL, int *pos = NULL)
  {
    int i = 1;
    uint8_t previous_switch_counter;
    char *ret = NULL;
    char *t;
    entry_key_t k;

    // internal node
  Again:
    do
    {

      previous_switch_counter = hdr.switch_counter;
      ret = NULL;

      if (IS_FORWARD(previous_switch_counter))
      {
        if (key < (k = records[0].key))
        {
          if ((t = (char *)hdr.leftmost_ptr) != records[0].ptr)
          {
            ret = t;
            continue;
          }
        }

        for (i = 1; records[i].ptr != NULL; ++i)
        {
          if (key < (k = records[i].key))
          {
            if ((t = records[i - 1].ptr) != records[i].ptr)
            {
              ret = t;
              break;
            }
          }
        }

        if (!ret)
        {
          ret = records[i - 1].ptr;
          continue;
        }
      }
    } while (hdr.switch_counter != previous_switch_counter);
    if ((t = (char *)hdr.sibling_ptr) != NULL)
    {
      if (key >= ((inner_page *)t)->hdr.highest)
      {
        // puts("error0");
        // exit(0);
        return t;
      }
    }

    if (ret)
    {
      if (key < ((inner_page *)ret)->hdr.lowest)
      {
        // printf("inner lowest key is %lu\n",((inner_page *)ret)->hdr.lowest);
        if (node && ret == (char *)node)
        {
          return NULL;
        }
        // puts("error1");
        // int k = 0;
        // inner_page *leaf = (inner_page *)ret;
        // while (leaf->records[k].ptr != NULL)
        // {
        //   printf("the key is %lu, ptr is %lu\n", leaf->records[k].key, leaf->records[k].ptr);
        //   k++;
        // }
        // printf("k is %d, level is %d\n", k, leaf->hdr.level);
        // printf("this is %lu\n", (uint64_t)ret-(uint64_t)base_addr);
        // exit(0);
        goto Again;
      }
      if (pos)
      {
        *pos = i - 1;
      }

      return ret;
    }
    else
    {
      if (pos)
      {
        *pos = i - 1;
      }
      return (char *)hdr.leftmost_ptr;
    }

    return NULL;
  }
  bool remove(btree *bt, entry_key_t key,
              bool only_rebalance = false, bool with_lock = true)
  {
    if (with_lock)
    {
      hdr.mtx->lock();
    }
    if (hdr.is_deleted)
    {
      if (with_lock)
      {
        hdr.mtx->unlock();
      }
      return false;
    }

    register int num_entries = count();
    // for(int k=0;k<num_entries;k++){
    //   printf("the key is %d\n",records[k].key);
    // }
    // This node is root
    if (this == (inner_page *)bt->root)
    {
      printf("the num in root is %d\n", num_entries);

      if (hdr.level > 0)
      {
        if (num_entries == 1 && !hdr.sibling_ptr)
        {

          bt->root = (inner_page *)hdr.leftmost_ptr;
          bt->height--;
          persist((char *)&(bt->root), sizeof(char *));
          // need recycle this node
          hdr.is_deleted = 1;
        }
      }

      // Remove the key from this node
      bool ret = remove_key(key);
      // register int num_entries = count();
      // printf("the num in root is %d\n",num_entries);
      if (with_lock)
      {
        hdr.mtx->unlock();
      }

      return true;
    }

    bool should_recycle = true;
    // check the node utilization
    if (num_entries > 1)
    {
      should_recycle = false;
    }

    // Remove the key from this node
    bool ret = remove_key(key);

    if (!should_recycle)
    {
      if (with_lock)
      {
        hdr.mtx->unlock();
      }
      return (hdr.leftmost_ptr == NULL) ? ret : true;
    }

    // Remove a key from the parent node
    entry_key_t deleted_key_from_parent = 0;
    bool is_leftmost_node = false;
    inner_page *left_sibling;
    bt->btree_delete_internal(key, (char *)this, hdr.level + 1,
                              &deleted_key_from_parent, &is_leftmost_node,
                              &left_sibling);

    if (is_leftmost_node)
    {
      if (with_lock)
      {
        hdr.mtx->unlock();
      }

      if (!with_lock)
      {
        hdr.sibling_ptr->hdr.mtx->lock();
      }
      hdr.sibling_ptr->remove(bt, hdr.sibling_ptr->records[0].key, true,
                              with_lock);
      if (!with_lock)
      {
        hdr.sibling_ptr->hdr.mtx->unlock();
      }
      return true;
    }

    if (with_lock)
    {
      left_sibling->hdr.mtx->lock();
    }

    while (left_sibling->hdr.sibling_ptr != this)
    {
      if (with_lock)
      {
        inner_page *t = left_sibling->hdr.sibling_ptr;
        left_sibling->hdr.mtx->unlock();
        left_sibling = t;
        left_sibling->hdr.mtx->lock();
      }
      else
        left_sibling = left_sibling->hdr.sibling_ptr;
    }
    // recycle

    left_sibling->hdr.sibling_ptr = hdr.sibling_ptr;
    persist((char *)&(left_sibling->hdr.sibling_ptr), 1);

    if (with_lock)
    {
      left_sibling->hdr.mtx->unlock();
      hdr.mtx->unlock();
    }

    return true;
  }
  inline bool remove_key(entry_key_t key)
  {
    hdr.mtx->lock();
    if (hdr.is_deleted)
    {
      hdr.mtx->unlock();
      return false;
    }
    // Set the switch_counter
    bool shift = false;
    int i;
    for (i = 0; records[i].ptr != NULL; ++i)
    {
      if (!shift && records[i].key == key)
      {
        records[i].ptr =
            (i == 0) ? (char *)hdr.leftmost_ptr : records[i - 1].ptr;
        shift = true;
      }

      if (shift)
      {
        records[i].key = records[i + 1].key;
        records[i].ptr = records[i + 1].ptr;
      }
    }

    if (shift)
    {
      --hdr.last_index;
      // puts("deleted!!!");
    }
    else
    {
      // puts("not deleted!!!");
    }
    hdr.mtx->unlock();
    return shift;
  }
};

class leaf_page
{
public:
  leaf_header hdr;                 // header in persistent memory, 48 bytes
  entry records[leaf_cardinality]; // slots in persistent memory, 16 bytes * n

public:
  friend class btree;

  void constructor(uint32_t level = 0)
  {
    hdr.constructor();
    for (int i = 0; i < leaf_cardinality; i++)
    {
      records[i].key = LONG_MAX;
      records[i].ptr = NULL;
    }

    hdr.level = level;
    records[0].ptr = NULL;
  }

  // this is called when tree grows
  void constructor(PMEMobjpool *pop, inner_page *left, entry_key_t key, inner_page *right,
                   uint32_t level = 0)
  {
    hdr.constructor();
    for (int i = 0; i < leaf_cardinality; i++)
    {
      records[i].key = LONG_MAX;
      records[i].ptr = NULL;
    }

    hdr.leftmost_ptr = left;
    hdr.level = level;
    records[0].key = key;
    records[0].ptr = (char *)right;
    records[1].ptr = NULL;

    hdr.last_index = 0;
  }

  inline int count()
  {
    uint8_t previous_switch_counter;
    int count = 0;

    do
    {
      previous_switch_counter = hdr.switch_counter;
      count = hdr.last_index + 1;

      while (count >= 0 && records[count].ptr != NULL)
      {
        if (IS_FORWARD(previous_switch_counter))
          ++count;
        else
          --count;
      }

      if (count < 0)
      {
        count = 0;
        while (records[count].ptr != NULL)
        {
          ++count;
        }
      }

    } while (previous_switch_counter != hdr.switch_counter);

    return count;
  }

  inline void insert_key(entry_key_t key, char *ptr,
                         int *num_entries, bool flush = true,
                         bool update_last_index = true)
  {
    // update switch_counter
    if (!IS_FORWARD(hdr.switch_counter))
      ++hdr.switch_counter;

    // FAST
    if (*num_entries == 0)
    { // this page is empty
      entry *new_entry = (entry *)&records[0];
      entry *array_end = (entry *)&records[1];
      new_entry->key = (entry_key_t)key;
      new_entry->ptr = (char *)ptr;

      array_end->ptr = (char *)NULL;

      if (flush)
      {
        persist(this, CACHE_LINE_SIZE);
      }
    }
    else
    {
      int i = *num_entries - 1, inserted = 0, to_flush_cnt = 0;
      records[*num_entries + 1].ptr = records[*num_entries].ptr;

      if (flush)
      {
        if ((uint64_t) & (records[*num_entries + 1]) % CACHE_LINE_SIZE == 0)
        {
          persist(&records[*num_entries + 1].ptr, sizeof(char *));
        }
      }

      // FAST
      for (i = *num_entries - 1; i >= 0; i--)
      {
        if (key < records[i].key)
        {
          records[i + 1].ptr = records[i].ptr;
          records[i + 1].key = records[i].key;

          if (flush)
          {
            uint64_t records_ptr = (uint64_t)(&records[i + 1]);

            int remainder = records_ptr % CACHE_LINE_SIZE;
            bool do_flush =
                (remainder == 0) ||
                ((((int)(remainder + sizeof(entry)) / CACHE_LINE_SIZE) == 1) &&
                 ((remainder + sizeof(entry)) % CACHE_LINE_SIZE) != 0);
            if (do_flush)
            {
              persist((void *)records_ptr, CACHE_LINE_SIZE);
            }
            else
              ++to_flush_cnt;
          }
        }
        else
        {
          records[i + 1].ptr = records[i].ptr;
          records[i + 1].key = key;
          records[i + 1].ptr = ptr;

          if (flush)
            persist(&records[i + 1], sizeof(entry));
          inserted = 1;
          break;
        }
      }
      if (inserted == 0)
      {
        records[0].ptr = (char *)hdr.leftmost_ptr;
        records[0].key = key;
        records[0].ptr = ptr;

        if (flush)
          persist(&records[0], sizeof(entry));
      }
    }

    if (update_last_index)
    {
      hdr.last_index = *num_entries;
      // persist(&hdr,1);
    }
    ++(*num_entries);
  }

  // Insert a new key - FAST and FAIR
  leaf_page *store(btree *bt, char *left, entry_key_t key, char *right, bool flush,
                   bool with_lock, int id = 1, leaf_page *invalid_sibling = NULL)
  {
    // printf("id is %d\n", id);
    if (with_lock)
    {
      hdr.mtx->lock();
    }
    if (hdr.is_deleted)
    {
      if (with_lock)
      {
        hdr.mtx->unlock();
        persist(&hdr.mtx, 64);
      }

      return NULL;
    }

    // If this node has a sibling node,

    if (hdr.sibling_ptr != 0)
    {
      // printf("access sibling id is %d\n", id);
      leaf_page *sibling_ptr = (leaf_page *)(hdr.sibling_ptr + (uint64_t)(bt->base));
      // printf("base is %lu\n", bt->base);
      // printf("access sibling is %lu\n", sibling_ptr);
      // Compare this key with the first key of the sibling
      if (key > sibling_ptr->hdr.lowest)
      {
        if (with_lock)
        {
          hdr.mtx->unlock();
          // persist(&hdr.rwlock, 64);
        }
        // printf("access sibling id is %d\n", id);

        return sibling_ptr
            ->store(bt, NULL, key, right, true, with_lock, id, invalid_sibling);
      }
    }

    register int num_entries = count();
    // printf("count is %d, max is %d\n", num_entries,leaf_cardinality - 1);
    // FAST
    if (num_entries < leaf_cardinality - 1)
    {
      insert_key(key, right, &num_entries, flush);

      if (with_lock)
      {
        hdr.mtx->unlock();
        // persist(&hdr.rwlock, 64);
      }
      // printf("finished id is %d\n", id);
      return this;
    }
    else
    {

      // FAIR
      // overflow
      // create a new node
      split_cnt++;
      leaf_page *sibling_ptr;
      uint64_t sibling_offset;
#ifdef PMDK
      clock_gettime(CLOCK_MONOTONIC, &start_timer);

      struct pobj_action actv[2];
      TOID(leaf_page)
      sibling = pmemobj_reserve(bt->pop, &actv[0], LEAF_PAGESIZE, 0);
      clock_gettime(CLOCK_MONOTONIC, &end_timer);
      time_m = (end_timer.tv_sec - start_timer.tv_sec) * 1000000000 + (end_timer.tv_nsec - start_timer.tv_nsec);
      time_cnt[id] += time_m;

      D_RW(sibling)->constructor(hdr.level);
      sibling_ptr = D_RW(sibling);
      sibling_offset = sibling.oid.off;
      // printf("the new node is %lu\n", sibling.oid.off);
      if (get_rand() == 1)
      {
        fail_cnt++;
        hdr.mtx->unlock();
        return this;
      }

#endif
#ifdef STM
#ifdef A_F
      sibling_offset = alloc->alloc_free(id);
      if (sibling_offset == 0)
      {
        sibling_offset = alloc->pm_alloc(id, LEAF_PAGESIZE);
      }
#else
      clock_gettime(CLOCK_MONOTONIC, &start_timer);
      sibling_offset = alloc->pm_alloc(id, LEAF_PAGESIZE);
      clock_gettime(CLOCK_MONOTONIC, &end_timer);
      time_m = (end_timer.tv_sec - start_timer.tv_sec) * 1000000000 + (end_timer.tv_nsec - start_timer.tv_nsec);
      time_cnt[id] += time_m;
#endif

      sibling_ptr = (leaf_page *)((uint64_t)(bt->base) + sibling_offset);
      // printf("the allocated node is %lu\n", sibling_offset);
      sibling_ptr->constructor(hdr.level);

#endif
#ifdef NV_MALLOC
      clock_gettime(CLOCK_MONOTONIC, &start_timer);
      sibling_ptr = (leaf_page *)nvm_reserve(LEAF_PAGESIZE);
      clock_gettime(CLOCK_MONOTONIC, &end_timer);
      time_m = (end_timer.tv_sec - start_timer.tv_sec) * 1000000000 + (end_timer.tv_nsec - start_timer.tv_nsec);
      time_cnt[id] += time_m;
      sibling_offset = (uint64_t)sibling_ptr - (uint64_t)(bt->base);

      sibling_ptr->constructor(hdr.level);
      if (get_rand() == 1)
      {
        fail_cnt++;
        hdr.mtx->unlock();
        return this;
      }
      //  if(get_rand()==1){
      //   return this;
      // }
      //  printf("sibling on new node is %lu\n", sibling_ptr->hdr.sibling_ptr);
#endif
      register int m = (int)ceil(num_entries / 2);
      entry_key_t split_key = records[m].key;

      // migrate half of keys into the sibling
      int sibling_cnt = 0;
      for (int i = m; i < num_entries; ++i)
      {
        sibling_ptr->insert_key(records[i].key, records[i].ptr,
                                &sibling_cnt, false);
      }

      sibling_ptr->hdr.sibling_ptr = hdr.sibling_ptr;
      sibling_ptr->hdr.lowest = split_key;
#ifdef STM
      clock_gettime(CLOCK_MONOTONIC, &start_timer);
      sibling_ptr->hdr.meta.usage = 1;
      sibling_ptr->hdr.meta.referenced = (uint64_t)this - (uint64_t)(alloc->master);
      clock_gettime(CLOCK_MONOTONIC, &end_timer);
      time_m = (end_timer.tv_sec - start_timer.tv_sec) * 1000000000 + (end_timer.tv_nsec - start_timer.tv_nsec);
      time_cnt[id] += time_m;
#endif
      persist_page(sibling_ptr, sizeof(leaf_page));

      // hdr.sibling_ptr = sibling_offset;
      // hdr.highest = split_key;
      // persist(&hdr, sizeof(hdr));

#ifdef STM
      clock_gettime(CLOCK_MONOTONIC, &start_timer);
      hdr.sibling_ptr = sibling_offset;
      persist(&hdr, sizeof(hdr));
      clock_gettime(CLOCK_MONOTONIC, &end_timer);
      time_m = (end_timer.tv_sec - start_timer.tv_sec) * 1000000000 + (end_timer.tv_nsec - start_timer.tv_nsec);
      time_cnt[id] += time_m;
#endif

#ifdef PMDK
      if (get_rand() == 1)
      {
        fail_cnt++;
        hdr.mtx->unlock();
        return this;
      }
      clock_gettime(CLOCK_MONOTONIC, &start_timer);
      pmemobj_set_value(bt->pop, &actv[1], &hdr.sibling_ptr, sibling_offset);

      // pmemobj_set_value(bt->pop,&actv[2],&hdr.highest,split_key);
      pmemobj_publish(bt->pop, actv, 2);
      clock_gettime(CLOCK_MONOTONIC, &end_timer);
      time_m = (end_timer.tv_sec - start_timer.tv_sec) * 1000000000 + (end_timer.tv_nsec - start_timer.tv_nsec);
      time_cnt[id] += time_m;
#endif
#ifdef STM
      // alloc->pm_valid(sibling_offset);
      clock_gettime(CLOCK_MONOTONIC, &start_timer);
      sibling_ptr->hdr.meta.valid = 1;
      sibling_ptr->hdr.meta.referenced = 0;
      if (key < split_key)
      {
        persist(&sibling_ptr->hdr, 1);
      }

      clock_gettime(CLOCK_MONOTONIC, &end_timer);
      time_m = (end_timer.tv_sec - start_timer.tv_sec) * 1000000000 + (end_timer.tv_nsec - start_timer.tv_nsec);
      time_cnt[id] += time_m;
      // printf("valid is %d, usage is %d, referenced is %d\n", sibling_ptr->hdr.meta.valid, sibling_ptr->hdr.meta.usage, sibling_ptr->hdr.meta.referenced);
#endif
#ifdef NV_MALLOC
      //  hdr.sibling_ptr = sibling_offset;

      //  persist(&hdr, sizeof(hdr));
      //        if(get_rand()==1){
      //   return this;
      // }
      clock_gettime(CLOCK_MONOTONIC, &start_timer);
      nvm_activate((void *)sibling_ptr, (void **)&hdr.sibling_ptr, (void *)sibling_ptr, NULL, NULL);
      clock_gettime(CLOCK_MONOTONIC, &end_timer);
      time_m = (end_timer.tv_sec - start_timer.tv_sec) * 1000000000 + (end_timer.tv_nsec - start_timer.tv_nsec);
      time_cnt[id] += time_m;
#endif
      // set to NULL
      if (IS_FORWARD(hdr.switch_counter))
        hdr.switch_counter += 2;
      else
        ++hdr.switch_counter;
      records[m].ptr = NULL;
      persist(&records[m], sizeof(entry));

      hdr.last_index = m - 1;
      persist(&hdr.last_index, sizeof(int16_t));

      num_entries = hdr.last_index + 1;

      leaf_page *ret;

      // insert the key
      if (key < split_key)
      {
        insert_key(key, right, &num_entries);
        ret = this;
      }
      else
      {
        sibling_ptr->insert_key(key, right, &sibling_cnt);
        ret = (leaf_page *)sibling_offset;
      }

      // Set a new root or insert the split key to the parent
      if (bt->height == 1)
      { // only one node can update the root ptr
        inner_page *new_root =
            new inner_page((inner_page *)this, split_key, (inner_page *)sibling_ptr, hdr.level + 1);
        // printf("the new root is %lu\n", new_root);
        bt->setNewRoot(new_root);
        if (with_lock)
        {
          hdr.mtx->unlock();
          persist(&hdr.mtx, 64);
        }
      }
      else
      {
        if (with_lock)
        {
          hdr.mtx->unlock();
          persist(&hdr.mtx, 64);
        }
        // printf("insert internal,\n");
        bt->btree_insert_internal(NULL, split_key, (char *)sibling_ptr,
                                  hdr.level + 1);
      }

      return ret;
    }
  }
  bool remove(btree *bt, entry_key_t key,
              int id = 0, bool with_lock = true)
  {
    if (with_lock)
    {
      hdr.mtx->lock();
    }
    if (hdr.is_deleted)
    {
      if (with_lock)
      {
        hdr.mtx->unlock();
      }
      return false;
    }

    register int num_entries = count();

    bool should_recycle = true;
    // check the node utilization
    if (num_entries > 1)
    {
      should_recycle = false;
    }

    // Remove the key from this node
    bool ret;

    if (!should_recycle)
    {
      ret = remove_key(key);
      if (with_lock)
      {
        hdr.mtx->unlock();
      }
      return (hdr.leftmost_ptr == NULL) ? ret : true;
    }
    // Remove a key from the parent node
    entry_key_t deleted_key_from_parent = 0;
    bool is_leftmost_node = false;
    inner_page *temp;

    bt->btree_delete_internal(key, (char *)this, hdr.level + 1,
                              &deleted_key_from_parent, &is_leftmost_node,
                              &temp);
    leaf_page *left_sibling = (leaf_page *)temp;
    if (is_leftmost_node)
    {
#ifdef STM
      hdr.meta.referenced = bt->leftbottom;
#endif
      hdr.is_deleted = 1;
      ret = remove_key(key);
      // bt->leftbottom = (uint64_t)this - (uint64_t)bt->base;
      // hdr.is_deleted = 1;
#ifdef PMDK
      // bt->leftbottom = (uint64_t)this - (uint64_t)bt->base;
      clock_gettime(CLOCK_MONOTONIC, &start_timer);

      struct pobj_action actv[2];

      TOID(leaf_page)
      t;
      t.oid = pmemobj_oid(this);
      // t.oid.off = hdr.sibling_ptr;
      pmemobj_defer_free(bt->pop, t.oid, &actv[0]);
      // pmemobj_set_value(bt->pop, &actv[1], &bt->leftbottom, (uint64_t)this - (uint64_t)bt->base);

      pmemobj_publish(bt->pop, &actv[0], 1);
      clock_gettime(CLOCK_MONOTONIC, &end_timer);
      time_m = (end_timer.tv_sec - start_timer.tv_sec) * 1000000000 + (end_timer.tv_nsec - start_timer.tv_nsec);
      time_cnt[id] += time_m;
#endif

#ifdef NV_MALLOC
      clock_gettime(CLOCK_MONOTONIC, &start_timer);
      nvm_free(this, NULL, NULL, NULL, NULL);
      clock_gettime(CLOCK_MONOTONIC, &end_timer);
      time_m = (end_timer.tv_sec - start_timer.tv_sec) * 1000000000 + (end_timer.tv_nsec - start_timer.tv_nsec);
      time_cnt[id] += time_m;
#endif

#ifdef STM
      clock_gettime(CLOCK_MONOTONIC, &start_timer);
      alloc->pm_free((uint64_t)this - (uint64_t)bt->base, id);
      clock_gettime(CLOCK_MONOTONIC, &end_timer);
      time_m = (end_timer.tv_sec - start_timer.tv_sec) * 1000000000 + (end_timer.tv_nsec - start_timer.tv_nsec);
      time_cnt[id] += time_m;
#endif
      // puts("recycled");
      if (with_lock)
      {
        hdr.mtx->unlock();
      }
      return true;
    }
    if (!left_sibling)
    {
      hdr.mtx->unlock();
      return false;
    }
    if (with_lock)
    {
      left_sibling->hdr.mtx->lock();
    }
    // // puts("deleted internal");
    while (left_sibling->hdr.sibling_ptr != (uint64_t)this - (uint64_t)bt->base)
    {
      if (with_lock)
      {
        leaf_page *t = (leaf_page *)(left_sibling->hdr.sibling_ptr + (uint64_t)bt->base);
        left_sibling->hdr.mtx->unlock();
        left_sibling = t;
        left_sibling->hdr.mtx->lock();
      }
      else
        left_sibling = (leaf_page *)(left_sibling->hdr.sibling_ptr + (uint64_t)bt->base);
    }
#ifdef STM
    hdr.meta.referenced = (uint64_t)left_sibling - (uint64_t)bt->base;
#endif
    hdr.is_deleted = 1;
    ret = remove_key(key);
// recycle
// puts("find leftsibling");
#ifdef PMDK
    clock_gettime(CLOCK_MONOTONIC, &start_timer);
    struct pobj_action actv[2];

    TOID(leaf_page)
    t;
    t.oid = pmemobj_oid(this);
    // t.oid.off = hdr.sibling_ptr;
    pmemobj_defer_free(bt->pop, t.oid, &actv[0]);
    pmemobj_set_value(bt->pop, &actv[1], &left_sibling->hdr.sibling_ptr, hdr.sibling_ptr);

    pmemobj_publish(bt->pop, &actv[0], 2);
    clock_gettime(CLOCK_MONOTONIC, &end_timer);
    time_m = (end_timer.tv_sec - start_timer.tv_sec) * 1000000000 + (end_timer.tv_nsec - start_timer.tv_nsec);
    time_cnt[id] += time_m;
#endif

#ifdef NV_MALLOC
    clock_gettime(CLOCK_MONOTONIC, &start_timer);
    nvm_free(this, (void **)&left_sibling->hdr.sibling_ptr, (void *)(hdr.sibling_ptr + (uint64_t)bt->base), NULL, NULL);
    clock_gettime(CLOCK_MONOTONIC, &end_timer);
    time_m = (end_timer.tv_sec - start_timer.tv_sec) * 1000000000 + (end_timer.tv_nsec - start_timer.tv_nsec);
    time_cnt[id] += time_m;
#endif

#ifdef STM
    clock_gettime(CLOCK_MONOTONIC, &start_timer);
    left_sibling->hdr.sibling_ptr = hdr.sibling_ptr;
    persist((char *)&(left_sibling->hdr.sibling_ptr), 1);
    alloc->pm_free((uint64_t)this - (uint64_t)bt->base, id);
    clock_gettime(CLOCK_MONOTONIC, &end_timer);
    time_m = (end_timer.tv_sec - start_timer.tv_sec) * 1000000000 + (end_timer.tv_nsec - start_timer.tv_nsec);
    time_cnt[id] += time_m;
#endif

    // left_sibling->hdr.sibling_ptr = hdr.sibling_ptr;
    // persist((char *)&(left_sibling->hdr.sibling_ptr), 1);
    // hdr.is_deleted = 1;
    // puts("recycled");
    if (with_lock)
    {
      left_sibling->hdr.mtx->unlock();
      hdr.mtx->unlock();
    }

    return true;
  }

  inline bool remove_key(entry_key_t key, int id = 0)
  {
    // Set the switch_counter
    if (IS_FORWARD(hdr.switch_counter))
      ++hdr.switch_counter;

    bool shift = false;
    bool del = false;
    int i;
    leaf_page *left_sibling = NULL;
    for (i = 0; records[i].ptr != NULL; ++i)
    {
      if (!shift && records[i].key == key)
      {
        shift = true;
        if (i == 0)
        {
          records[i].ptr = NULL;
        }
        else
        {
          records[i].ptr = records[i - 1].ptr;
          shift = true;
        }
      }
      if (shift)
      {

        records[i].key = records[i + 1].key;
        records[i].ptr = records[i + 1].ptr;

        // flush
        uint64_t records_ptr = (uint64_t)(&records[i]);
        int remainder = records_ptr % CACHE_LINE_SIZE;
        bool do_flush =
            (remainder == 0) ||
            ((((int)(remainder + sizeof(entry)) / CACHE_LINE_SIZE) == 1) &&
             ((remainder + sizeof(entry)) % CACHE_LINE_SIZE) != 0);
        if (do_flush)
        {
          persist((void *)records_ptr, CACHE_LINE_SIZE);
        }
      }
    }

    if (shift)
    {
      --hdr.last_index;
    }
    return shift;
  }
  // bool remove(btree *bt, entry_key_t key, int id = 0)
  // {
  //   // pthread_rwlock_wrlock(hdr.rwlock);
  //   if (hdr.sibling_ptr != 0)
  //   {
  //     leaf_page *sibling_ptr = (leaf_page *)(hdr.sibling_ptr + (uint64_t)(bt->base));
  //     // Compare this key with the first key of the sibling
  //     if (key > sibling_ptr->hdr.lowest)
  //     {
  //       // pthread_rwlock_unlock(hdr.rwlock);
  //       persist(&hdr.mtx, 64);
  //       // printf("key and sib key %lu, %lu\n",key,sibling_ptr->hdr.lowest);
  //       // printf("the first pointer %lu\n",sibling_ptr->records[0].key);
  //       // return false;
  //       return sibling_ptr->remove(bt, key,id);
  //     }
  //   }
  //   bool ret = remove_recycle(bt,key,id);

  //   // pthread_rwlock_unlock(hdr.rwlock);

  //   return ret;
  // }
};

/*
 * class btree
 */
void btree::constructor()
{
#ifdef PMDK
  pop = pool;
  if (!leftbottom)
  {
    TOID(leaf_page)
    temp;
    POBJ_NEW(pop, &temp, leaf_page, NULL, NULL);
    leftbottom = temp.oid.off;
    D_RW(temp)->constructor();
    persist_page(D_RW(temp), LEAF_PAGESIZE);
    root = (inner_page *)D_RW(temp);
    // printf("the first node is %lu\n", leftbottom);
  }

#endif

#ifdef STM
  if (alloc->master->leftmost == 0)
  {
    alloc->master->leftmost = alloc->pm_alloc(0, LEAF_PAGESIZE);
    leftbottom = alloc->master->leftmost;
    leaf_page *temp = (leaf_page *)((uint64_t)(base) + leftbottom);
    // base_addr = base;
    temp->constructor();
    persist_page(temp, LEAF_PAGESIZE);
    // printf("the first node is %lu\n", leftbottom);
  }
  else
  {
    // puts("123");
    leftbottom = alloc->master->leftmost;
    // printf("the leftbottom node is %lu\n", leftbottom);
    // base_addr = base;
  }

#endif
#ifdef NV_MALLOC
  leaf_page *temp = (leaf_page *)nvm_reserve(LEAF_PAGESIZE);
  leftbottom = (uint64_t)temp - (uint64_t)base;
  temp->constructor();
  persist_page(temp, LEAF_PAGESIZE);
#endif
  height = 1;
}

void btree::setNewRoot(inner_page *new_root)
{
  root = new_root;
  persist(&root, 64);
  ++height;
}

// insert the key in the leaf node
void btree::btree_insert(entry_key_t key, char *right, int id)
{
  if (height == 1)
  {
    leaf_page *leaf = (leaf_page *)((uint64_t)(base) + leftbottom);

    if (!leaf->store(this, NULL, key, right, true, true, id))
    { // store

      btree_insert(key, right, id);
    }
  }
  else
  {
    inner_page *p = root;
    while (p->hdr.leftmost_ptr != NULL)
    {
      p = (inner_page *)p->linear_search(key);
    }
    leaf_page *leaf = (leaf_page *)p;
    if (!leaf->store(this, NULL, key, right, true, true, id))
    { // store
      btree_insert(key, right, id);
    }
  }
}

void btree::btree_delete_internal(entry_key_t key, char *ptr, uint32_t level,
                                  entry_key_t *deleted_key,
                                  bool *is_leftmost_node, inner_page **left_sibling)
{
  if (level > root->hdr.level)
    return;

  inner_page *p = root;

  while (p->hdr.level > level)
  {
    p = (inner_page *)p->linear_search(key);
  }

  if ((char *)p->hdr.leftmost_ptr == ptr)
  {
    // p->hdr.mtx->lock();
    // if(p->hdr.is_deleted){
    //   p->hdr.mtx->unlock();
    //   return;
    // }
    // // if(p->records[1].ptr==NULL){
    // //   p->hdr.leftmost_ptr=NULL;
    // //   p->hdr.mtx->unlock();
    // //   return;
    // // }
    // // puts("leftmost!");
    // *is_leftmost_node = true;
    // p->hdr.leftmost_ptr = (inner_page *)p->records[0].ptr;
    // p->records[0].key = p->records[1].key;
    // for (int i = 0; p->records[i].ptr != NULL; ++i)
    // {
    //   p->records[i].key = p->records[i + 1].key;
    //   p->records[i].ptr = p->records[i + 1].ptr;
    // }

    // p->hdr.mtx->unlock();
    *left_sibling = NULL;
    return;
  }

  *is_leftmost_node = false;

  for (int i = 0; p->records[i].ptr != NULL; ++i)
  {
    if (p->records[i].ptr == ptr)
    {
      if (i == 0)
      {
        if ((char *)p->hdr.leftmost_ptr != p->records[i].ptr)
        {
          p->records[i].ptr = (char *)p->hdr.leftmost_ptr;
          *deleted_key = p->records[i].key;
          *left_sibling = p->hdr.leftmost_ptr;

          // p->remove_key(*deleted_key);
          break;
        }
      }
      else
      {
        if (p->records[i - 1].ptr != p->records[i].ptr)
        {
          *deleted_key = p->records[i].key;
          *left_sibling = (inner_page *)p->records[i - 1].ptr;
          p->records[i].ptr = p->records[i - 1].ptr;
          // p->remove_key(*deleted_key);
          break;
        }
      }
    }
  }
}

void btree::btree_delete(entry_key_t key, int id = 0)
{
  if (height == 1)
  {
    leaf_page *leaf = (leaf_page *)((uint64_t)(base) + leftbottom);
    leaf->remove(this, key, id);
  }
  else
  {
    inner_page *p = root;

    while (p->hdr.leftmost_ptr != NULL)
    {
      p = (inner_page *)p->linear_search(key);
    }
    leaf_page *leaf = (leaf_page *)p;
    leaf->remove(this, key, id);
  }
}

int get_status(leaf_page *leaf)
{
  if (leaf->hdr.meta.valid == 0 && leaf->hdr.meta.referenced == NULL && leaf->hdr.meta.usage != 0)
  {
    return 0; // constructing A
  }
  if (leaf->hdr.meta.valid == 0 && leaf->hdr.meta.referenced != NULL && leaf->hdr.meta.usage != 0)
  {
    return 1; // constructed
  }
  if (leaf->hdr.meta.valid == 1 && leaf->hdr.meta.referenced == NULL && leaf->hdr.meta.usage != 0)
  {
    return 2; // in use
  }
  if (leaf->hdr.meta.valid == 1 && leaf->hdr.meta.referenced == NULL && leaf->hdr.meta.usage == 0)
  {
    return 3; // empty
  }
  if (leaf->hdr.meta.valid == 1 && leaf->hdr.meta.referenced != NULL && leaf->hdr.meta.usage == 0)
  {
    return 4; // to be freed
  }
  if (leaf->hdr.meta.valid == 1 && leaf->hdr.meta.referenced != NULL && leaf->hdr.meta.usage != 0)
  {
    return 5; // constructing B
  }
}

void btree::recover(int thread)
{
  height = 1;
  // leaf_page *init = (leaf_page *)((uint64_t)base + leftbottom);

#ifndef STM
  // printf("leftbottom is %lu\n", leftbottom);
  leaf_page *current = (leaf_page *)((uint64_t)base + leftbottom);
  if (current->hdr.sibling_ptr == 0)
  {
    puts("only one node");
    return;
  }
  uint64_t count = 0;
  int i = 0;
  while (current)
  {
    count++;
    current->hdr.mtx = new std::mutex();

    if (height == 1)
    {
      inner_page *new_root =
          new inner_page((inner_page *)current, 0, NULL, 1);
      setNewRoot(new_root);
    }
    else
    {
      btree_insert_internal(NULL, current->hdr.lowest, (char *)current, 1);
    }

    if (current->hdr.sibling_ptr == 0)
    {
      break;
    }
    else
    {
      range_node_cnt++;
      current = (leaf_page *)((uint64_t)base + current->hdr.sibling_ptr);
    }
  }
  // printf("%lu nodes traversed\n", count);
#else
  leaf_page *init = (leaf_page *)((uint64_t)base + leftbottom);
  bool root_constructed = false;
  int status_leftmost = get_status(init);
  while (!root_constructed)
  {
    if (status_leftmost == 2)
    {
      init->hdr.mtx = new std::mutex();
      inner_page *new_root =
          new inner_page((inner_page *)init, 0, NULL, 1);
      setNewRoot(new_root);
      root_constructed = true;
    }
    else
    {
      init = (leaf_page *)(init->hdr.sibling_ptr + (uint64_t)base);
    }
  }

#pragma omp parallel for num_threads(thread)
  for (int i = 0; i < 32; ++i)
  {
    int s = 0;
    int offset = *(uint64_t *)((uint64_t)base + i * alloc->node_per_area * LEAF_PAGESIZE);
    leaf_page *start = (leaf_page *)((uint64_t)base + i * alloc->node_per_area * LEAF_PAGESIZE + LEAF_PAGESIZE);
    // printf("start node is %lu\n", i * alloc->node_per_area * LEAF_PAGESIZE + LEAF_PAGESIZE);
    // printf("offset is %lu\n", i * alloc->node_per_area * LEAF_PAGESIZE);
    if (start == init)
    {
      s = 1;
    }

    for (int i = s; i < offset; i++)
    {
      leaf_page *current = start + i;
      range_node_cnt++;
      int status = get_status(current);
      bool reconstruct = false;
      if (status == 1 || status == 4)
      {
        leaf_page *left = (leaf_page *)((uint64_t)current->hdr.meta.referenced + (uint64_t)base);
        if (left->hdr.sibling_ptr == (uint64_t)current - (uint64_t)base)
        {
          reconstruct = true;
        }
      }
      if (status == 2)
      {
        reconstruct = true;
      }
      if (reconstruct)
      {
        current->hdr.mtx = new std::mutex();
        btree_insert_internal(NULL, current->hdr.lowest, (char *)current, 1);
      }
      else
      {
        alloc->pm_free((uint64_t)current - (uint64_t)base, omp_get_thread_num());
      }
    }
  }
#endif

  // puts("recovered!");
}
// store the key into the node at the given level
void btree::btree_insert_internal(char *left, entry_key_t key, char *right,
                                  uint32_t level)
{
  if (level > ((inner_page *)root)->hdr.level)
    return;

  inner_page *p = (inner_page *)this->root;

  while (p->hdr.level > level)
    p = (inner_page *)p->linear_search(key);

  if (!p->store(this, NULL, key, right, true, true))
  {
    btree_insert_internal(left, key, right, level);
  }
}
void btree::check()
{
  if (!leftbottom)
  {
    puts("empty");
    return;
  }

  leaf_page *leaf = (leaf_page *)((uint64_t)(base) + leftbottom);
  int i = 0;
  printf("the %dth leaf is %lu\n", i, leftbottom);
  // int k = 0;
  // while (leaf->records[k].ptr)
  // {
  //   printf("the key is %lu\n", leaf->records[k].key);
  //   k++;
  // }
  // if(k==0){
  //   puts("empty");
  // }
  while (leaf->hdr.sibling_ptr)
  {

    leaf = (leaf_page *)((uint64_t)(base) + leaf->hdr.sibling_ptr);
    i++;
    printf("the %dth leaf is %lu\n", i, (uint64_t)leaf - (uint64_t)base);
  }
}
