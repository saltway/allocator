#include "nvm_malloc.h"
#include <cstring>
#include <sstream>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <random>
#include <string>
#include <sys/time.h>
#include <thread>
#include <vector>
#include <libpmemobj.h>
#include "stallocator_dis.h"
// #include "makalu.h"

std::vector<uint64_t> workerTimes;
uint64_t allocation_size_min = 64;
uint64_t allocation_size_max = 64;
int page_size = 128;
pm_allocator *alloc;
int op_number = 100000;
int n_threads = 32;
double load_factor = 1;
PMEMobjpool *pop;
char *persistent_path;
int flag = 1;
void clear_cache()
{
    // Remove cache
    int size = 256 * 1024 * 1024;
    char *garbage = new char[size];
    for (int i = 0; i < size; ++i)
        garbage[i] = i;
    for (int i = 100; i < size; ++i)
        garbage[i] += garbage[i - 100];
    delete[] garbage;
}

typedef struct
{
    void *previous;
    void *next;
    void *data;
} node_t;
struct timer
{
    struct timeval t_start = {0, 0};
    struct timeval t_end = {0, 0};
    inline void start()
    {
        gettimeofday(&t_start, nullptr);
    }
    inline uint64_t stop()
    {
        gettimeofday(&t_end, nullptr);
        return ((uint64_t)t_end.tv_sec - t_start.tv_sec) * 1000000ul + (t_end.tv_usec - t_start.tv_usec);
    }
};

inline void execute_in_pool(std::function<void(int)> func, size_t n_workers)
{
    std::vector<std::thread> threadpool;
    threadpool.reserve(n_workers);
    for (size_t i = 0; i < n_workers; ++i)
        threadpool.push_back(std::thread(func, i));
    for (auto &thread : threadpool)
        thread.join();
}

void worker(int id)
{
#ifdef NVMALLOC
    volatile char *pointerlist[op_number];
#endif
#ifdef MAKALU
    void *p[op_number];
#endif
#ifdef STM
    volatile uint64_t offsetlist[op_number];
#endif
#ifdef PMDK
    PMEMoid pmdklist[op_number];
#endif

    timer timer;
    // std::default_random_engine generator;
    // std::uniform_int_distribution<uint64_t> distribution(allocation_size_min, allocation_size_max);
    // auto randomSize = std::bind(distribution, generator);
    timer.start(); // allocate

    for (uint64_t i = 0; i < op_number; ++i)
    {
#ifdef NVMALLOC
        pointerlist[i] = (volatile char *)nvm_reserve(page_size);
        memset((void *)pointerlist[i], 5, 64);
        nvm_activate((void *)pointerlist[i], NULL, NULL, NULL, NULL);
#endif
#ifdef STM
        if (flag == 0)
        {
            offsetlist[i] = alloc->pm_alloc(id, page_size);
            alloc->pm_valid(offsetlist[i]);
        }
        else
        {
            offsetlist[i] = alloc->get_offset(i, id, page_size);
        }

        // offsetlist[i] = alloc->get_offset(i,id,page_size);
        // printf("id is :%d, the offset is %lu\n",id,offsetlist[i] );
// printf("id is :%d, the allocated offset is %lu\n",id,offsetlist[i] );
#endif
#ifdef PMDK
        // pmemobj_alloc(pop,&pmdklist[i],page_size,0, NULL,NULL);
        struct pobj_action actv[1];
        pmdklist[i] = pmemobj_reserve(pop, &actv[0], page_size, 0);
        pmemobj_publish(pop, &actv[0], 1);
#endif
    }
    if (flag == 0)
    {
        goto Result;
    }
    // clear_cache();
    timer.start(); // free
    for (uint64_t i = 0; i < op_number; ++i)
    {
#ifdef NVMALLOC
        nvm_free((void *)pointerlist[i], NULL, NULL, NULL, NULL);
#endif
#ifdef STM
        alloc->pm_free(offsetlist[i], id);
        // printf("id is :%d, the free offset is %lu\n",id,offsetlist[i] );
#endif
#ifdef PMDK
        pmemobj_free(&pmdklist[i]);
#endif
    }
    if (flag == 1)
    {
        goto Result;
    }
    timer.start(); // allocate after free
    for (int i = 0; i < op_number; ++i)
    {
#ifdef NVMALLOC
        pointerlist[i] = (volatile char *)nvm_reserve(page_size);
        memset((void *)pointerlist[i], 5, 64);
        nvm_activate((void *)pointerlist[i], NULL, NULL, NULL, NULL);
#endif
#ifdef STM
        offsetlist[i] = alloc->alloc_free(id);
        alloc->pm_valid(offsetlist[i]);
        // printf("id is :%d, the reallocate offset is %lu\n",id,offsetlist[i] );
#endif
#ifdef PMDK
        struct pobj_action actv[1];
        pmdklist[i] = pmemobj_reserve(pop, &actv[0], page_size, 0);
        pmemobj_publish(pop, &actv[0], 1);
#endif
    }
    if (flag == 2)
    {
        goto Result;
    }
// save result
Result:
    workerTimes[id] = timer.stop();
}

int main(int argc, char **argv)
{
    char *input_path = (char *)std::string("../sample_input.txt").data();
    int option = 0;
    int c;
    while ((c = getopt(argc, argv, "n:t:i:p:o:")) != -1)
    {
        switch (c)
        {
        case 'n':
            op_number = atoi(optarg);
            break;
        case 't':
            n_threads = atoi(optarg);
            break;
        case 'i':
            input_path = optarg;
        case 'p':
            persistent_path = optarg;
        case 'o':
            flag = atoi(optarg);
        default:
            break;
        }
    }

    // printf("ops number: %d, thread number: %d, flag is %d\n",op_number, n_threads,flag);
    workerTimes.resize(n_threads, 0);
#ifdef NVMALLOC
    nvm_initialize("/mnt/ywpmem0", 0);

    //  nvm_initialize("/home/yw", 0);
#endif
#ifdef STM
    alloc = new pm_allocator();
    // alloc->constructor("/mnt/ywpmem0/data0", page_size * n_threads * op_number * load_factor + page_size * 32);
    alloc->constructor("/mnt/ywpmem0/data0", 16000000000, n_threads, page_size);
    // puts("allocator initialized");

#endif
#ifdef PMDK
    pop = pmemobj_create("/mnt/ywpmem0/data1", "mytree_ll", 16000000000,
                         0666);
#endif
#ifdef MAKALU
    __map_persistent_region();
    void *ret = MAK_start(&__nvm_region_allocator);

#endif
    execute_in_pool(worker, n_threads);
    uint64_t avg = 0;
    for (auto t : workerTimes)
        avg += t;
    avg /= n_threads;
    // std::cout << "average time:" << avg << std::endl;
    std::cout << "throughput (MOPS): " << op_number * (double)n_threads / avg << std::endl;
#ifdef STM
    // alloc->check(op_number * n_threads);
#endif

    return 0;
}
