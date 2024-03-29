#include <cassert>
#include <climits>
#include <fstream>
#include <future>
#include <iostream>
#include <math.h>
#include <mutex>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <vector>
#include <libpmemobj.h>
#include <libpmem.h>
#include <immintrin.h>
#include <malloc.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <shared_mutex>
#include <jemalloc/jemalloc.h>
#define CELL_NUMBER 64
static inline void stm_persist(void *data, int len = 1)
{
    // asm volatile("sfence" ::: "memory");
    asm volatile("clwb %0"
                 : "+m"(*(volatile char *)data));
    //  pmemobj_flush(pop, data, len);
    //  pmem_flush(data, len);

    // asm volatile(".byte 0x66; xsaveopt %0"
    //              : "+m"(*(volatile char *)data));
    asm volatile("sfence" ::
                     : "memory");
    //  printf("the address to be persisted %lu\n",data);
}
struct proxy
{
    uint64_t offset = 0;
    void *next = NULL;
    //  uint64_t dummy[6];
};
struct list
{
    // uint64_t dummy0[8];
    std::mutex *lock = new std::mutex();
    // uint64_t dummy1[14];
    proxy *header = NULL;
    // uint64_t dummy2[6];
    // proxy *test = NULL;
    //
};
struct meta_template
{
    uint64_t valid : 1;
    uint64_t usage : 15;
    uint64_t referenced : 48;
};
struct node_template
{
    uint64_t valid : 16;
    uint64_t referenced : 48;
    uint64_t usage;
    uint64_t next;
    uint64_t dummy[29];
};
struct metanode
{
    uint64_t offset;
    uint64_t max_node_cnt;
    uint64_t recyle;
    uint64_t leftmost;
    uint64_t dummy[28];
};

int get_status(meta_template *meta)
{
    if (meta->valid == 1)
    {
        if (meta->referenced == NULL)
        {
            if (meta->usage == 0)
            {
                return 0; // available
            }
            else
            {
                return 1; // constructing
            }
        }
        else
        {
            return 2; // constructed
        }
    }
    else
    {
        if (meta->referenced == NULL)
        {
            if (meta->usage == 0)
            {
                return 3; // empty and not freed
            }
            else
            {
                return 4; // in use
            }
        }
        else
        {
            return 5; // to be deleted
        }
    }
}

class pm_allocator
{
public:
    metanode *master;
    std::mutex *mtx[CELL_NUMBER];
    list *free_list_int;
    uint64_t node_per_area;
    int max_threads;
    proxy **free_list;
    std::mutex **mtx_free;
    proxy *shadow_list[CELL_NUMBER];
    proxy *empty_list[CELL_NUMBER];
    uint64_t cnt;

public:
    void constructor(const char *, uint64_t, int, int, bool);
    uint64_t pm_alloc(int, int);
    uint64_t alloc_free(int);
    uint64_t get_offset(uint64_t, uint64_t, int);
    bool get_range(int, uint64_t *, uint64_t *, int, int);
    void pm_valid(uint64_t);
    void pm_free(uint64_t, int);
    void recover();
    void check(int, int);
};

void pm_allocator::constructor(const char *file, uint64_t size, int n_threads, int page_size, bool recover = true)
{

    if (!access(file, F_OK))
    {
        int fd = open(file, O_RDWR | O_CREAT, 0666);
        master = (metanode *)mmap(NULL, size, PROT_WRITE | PROT_READ, MAP_SHARED, fd, 0);

        // int quotient = (uint64_t)master%256;
        // printf("the quotion is %d\n",quotient);
    }
    else
    {
        puts("error with no files");
        exit(0);
    }
    master->max_node_cnt = size / page_size;
    node_per_area = master->max_node_cnt / CELL_NUMBER;
    cnt = 0;
    if (recover)
    {
        mtx_free = (std::mutex **)malloc(CELL_NUMBER * 8);
        free_list = (proxy **)malloc(CELL_NUMBER * 8);
        free_list_int = (list *)malloc(CELL_NUMBER * sizeof(list));
        for (int i = 0; i < CELL_NUMBER; i++)
        {
            free_list_int[i].header = new proxy();
            free_list[i] = new proxy();
            
            
            // shadow_list[i] = NULL;
            // empty_list[i] = NULL;
            mtx[i] = new std::mutex();
            mtx_free[i] = new std::mutex();
            free_list_int[i].lock = new std::mutex();
            master->offset = 0;
            master->leftmost = 0;
            max_threads = n_threads;
            memset((void *)((uint64_t)master + i * node_per_area * page_size), 0, 256);
        }
        // printf("the header inside alloc is %lu\n",free_list[0]);
    }
    else
    {
        // puts("new");
        max_threads = n_threads;
        // void *t = malloc(n_threads * 8);
        // free_list = (proxy **)t;
        master->offset = 0;
        if (master->offset != 0)
        {
            puts("error");
            // recover();
        }
        else
        {

            for (int i = 0; i < CELL_NUMBER; i++)
            {
                free_list[i] = new proxy();
                // free_list_int[i].header = new proxy();
                // free_list_int[i].lock = new std::mutex();
                // shadow_list[i] = NULL;
                // empty_list[i] = NULL;
                mtx[i] = new std::mutex();
                mtx_free[i] = new std::mutex();
                // mtx[i] = new std::mutex();
                memset((void *)((uint64_t)master + i * node_per_area * page_size), 0, 256);
                // master->offset = 0;
            }
            // for (int i = 0; i < 32; i++)
            // {
            //     printf("the data on the %dth area is %d\n", i, master->offset[i]);
            // }
        }
    }
    // printf("the max thread num is %d\n",max_threads);
};
uint64_t pm_allocator::alloc_free(int id = 0)
{

    // printf("id: %d\n",id);

    int area;
    if (max_threads >= CELL_NUMBER)
    {
        area = id % CELL_NUMBER;
    }
    else
    {
        area = ((double)CELL_NUMBER / max_threads) * id;
    }

Retry:
    free_list_int[area].lock->lock();
    proxy *temp = free_list_int[area].header;
    // printf("next: %d\n",temp->next);
    if (temp == NULL)
    {
        puts("error");
        exit(0);
        free_list_int[area].lock->unlock();
        //  return 0;
        area = (area + 1) % CELL_NUMBER;

        if (area == id % CELL_NUMBER)
        {
            // puts("no space in free");
            // free_list_int[area]->lock->unlock();
            return 0;
        }
        goto Retry;
    }
    // free_list_int[area]->lock->lock();
    free_list_int[area].header->next = (proxy *)temp->next;

    // if (!__atomic_compare_exchange_n(&free_list[off], &temp, temp->next, 1, 1, 0))
    // {
    //     goto Retry;
    // }
    uint64_t ret = temp->offset;
    // free(temp);
    free_list_int[area].lock->unlock();
    return ret;
}
uint64_t pm_allocator::pm_alloc(int id, int page_size)
{
    uint64_t ret;
    // ret = alloc_free(id);
    // if (ret)
    // {
    //     return ret;
    // }
    // printf("the id is %d\n", id);
    int area;
    if (max_threads >= CELL_NUMBER)
    {
        area = id % CELL_NUMBER;
    }
    else
    {
        area = ((double)CELL_NUMBER / max_threads) * id;
    }

    mtx[area]->lock();
    //  printf("the occupied is %d\n", master->offset[off] - page_size - off * master->max_node_cnt * 8);
    while (*(uint64_t *)((uint64_t)master + area * node_per_area * page_size) == node_per_area - 1)
    {
        mtx[area]->unlock();
        area = (area + 1) % CELL_NUMBER;
        mtx[area]->lock();
        // printf("id : %d the next off is %d\n", id, area);
        if (area == id % CELL_NUMBER)
        {
            mtx[area]->unlock();
            puts("no space");
            return 0;
        }
    }

    ret = *(uint64_t *)((uint64_t)master + area * node_per_area * page_size);
    // printf("id is %d,ret is : %d \n",area, ret);
    *(uint64_t *)((uint64_t)master + area * node_per_area * page_size) += 1;
    stm_persist((uint64_t *)((uint64_t)master + area * node_per_area * page_size));
    // proxy *temp = (proxy *)malloc(sizeof(proxy));
    // temp->next = shadow_list[area];
    // shadow_list[area] = temp;
    mtx[area]->unlock();
    return area * node_per_area * page_size + ret * page_size + sizeof(metanode);
};

void pm_allocator::pm_valid(uint64_t offset)
{
    meta_template temp;
    temp.valid = 1;
    temp.referenced = 0;
    // puts("1");
    meta_template *node = (meta_template *)((uint64_t)master + offset);

    // puts("2");
    mempcpy(node, &temp, 8);
    // printf("the pointer is %lu\n",node);
    //  if(*(uint16_t *)((uint64_t)master + offset) ==0)
    //  {
    //      puts("valid error");
    //      // exit(0);
    //  }
    stm_persist(node);
};

void pm_allocator::pm_free(uint64_t offset, int id = 0)
{
    // meta_template meta_temp;
    // meta_temp.valid = 0;
    // meta_temp.referenced = 0;

    // node_template *node = (node_template *)((uint64_t)master + offset);
    // node->referenced = 0;
    // node->valid = 0;
    // mempcpy(node, &meta_temp, 8);
    // if(*(uint16_t *)((uint64_t)master + offset) ==0)
    // {
    //     printf("%d th thread error offset is %li\n",id, offset);
    //     puts("error");

    // }
    // *(uint64_t *)((uint64_t)master + offset) = 0;
    //  memset((void *)((uint64_t)master + offset), 0, 8);
    int area;
    if (max_threads >= CELL_NUMBER)
    {
        area = id % CELL_NUMBER;
    }
    else
    {
        area = ((double)CELL_NUMBER / max_threads) * id;
    }

    // printf("the area is %d\n",area);

    // normal lock

    // mtx_free[area]->lock();
    // proxy *pnode = (proxy *)malloc(sizeof(proxy));
    // pnode->offset = offset;
    // pnode->next = free_list[area]->next;
    // free_list[area]->next = pnode;
    // mtx_free[area]->unlock();

   free_list_int[area].lock->lock();
    //aligned_alloc(64,sizeof(proxy));
    proxy *pnode = (proxy *)malloc(sizeof(proxy));
    pnode->offset = offset;
    pnode->next = free_list_int[area].header->next;
    free_list_int[area].header->next = pnode;
    free_list_int[area].lock->unlock();
    // Retry:

    //     if (_xbegin() == _XBEGIN_STARTED) {
    //         // Transactional code here
    //         pnode->next = free_list[id];
    //          free_list[id] = pnode;
    //         _xend();  // End the transaction
    //     } else {
    //         goto Retry;
    //         // Transaction aborted
    //     }

    //  proxy *pnode = (proxy *)malloc(sizeof(proxy));
    //  pnode->offset = offset;

    // Retry:
    //     proxy *temp = free_list[id];
    //     pnode->next = temp;
    //     if (!__atomic_compare_exchange_n(&free_list[id], &temp, pnode, 1, 1, 0))
    //     {
    //          goto Retry;
    //     }
    // printf("the header is %lu\n",free_list[0]);
};
uint64_t pm_allocator::get_offset(uint64_t offset, uint64_t id, int page_size)
{
    return offset * page_size + id % CELL_NUMBER * node_per_area * page_size + page_size + (id / CELL_NUMBER) * 0.5 * node_per_area * page_size;
};
bool pm_allocator::get_range(int id, uint64_t *start, uint64_t *end, int page_size, int max_threads)
{
    if (max_threads == 32)
    {
        *start = id * node_per_area * page_size + page_size;
        *end = (id + 1) * node_per_area * page_size - page_size;
    }
    else
    { // 64
        *start = get_offset(0, id, page_size);
        *end = *start + 0.5 * node_per_area * page_size;
    }
};
void pm_allocator::check(int number, int page_size)
{

    uint64_t sum = 0;
    for (int i = 0; i < max_threads; i++)
    {
        sum += *(uint64_t *)((uint64_t)master + i * node_per_area * page_size);
    }
    if (sum == number)
    {
        printf("%ld nodes allocated \n", sum);
        puts("allocation successful");
    }
    else
    {
        printf("%ld nodes allocated \n", sum);
        puts("error");
    }
    for (int i = 0; i < max_threads; i++)
    {
        for (uint64_t j = 0; j < node_per_area - 1; j++)
        {
            node_template *node = (node_template *)((uint64_t)master + i * node_per_area * page_size + j * page_size + page_size);
            if (!node->valid)
            {
                puts("error");
                exit(0);
            }
        }
    }
    puts("validation successful");
    int node_count = 0;
    for (int i = 0; i < 64; i++)
    {
        if (free_list[i] == NULL)
        {
            printf("the %d th list is usage\n", i);
        }
        else
        {
            int temp_cnt = 0;
            proxy *temp = free_list[i];
            while (temp != NULL)
            {
                temp_cnt++;
                temp = (proxy *)temp->next;
            }
            node_count += temp_cnt;
            printf("the %d th list has %d nodes \n", i, temp_cnt);
        }
    }
    printf("%d nodes freed \n", node_count);
};