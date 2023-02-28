// #ifndef DIS
// #include "ztree_bitmap_lockbit.h"
// #else
// #include "ztree_bitmap_lockbit_distributed.h"
// #endif
// #define DRAM
// #ifdef PERSISTENT
// #include "fast_p.h"
// #else
// #include "fast_s.h"
// #endif
#include "fast_s.h"

/*
 *  *file_exists -- checks if file exists
 *   */
static inline int file_exists(char const *file) { return access(file, F_OK); }
typedef struct
{
    void *previous;
    void *next;
    void *data;
} node_t;
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

// MAIN
int main(int argc, char **argv)
{
    // Parsing arguments
    int numData = 0;
    int n_threads = 1;
    char *input_path = (char *)std::string("../sample_input.txt").data();
    char *persistent_path;
    int option = 0;

    srand(time(NULL));
    int c;
    while ((c = getopt(argc, argv, "n:w:t:i:p:o:")) != -1)
    {
        switch (c)
        {
        case 'n':
            numData = atoi(optarg);
            break;
        case 't':
            n_threads = atoi(optarg);
            break;
        case 'i':
            input_path = optarg;
        case 'p':
            persistent_path = optarg;
        case 'o':
            option = atoi(optarg);
        default:
            break;
        }
    }
    for (int i = 0; i < 64; i++)
    {
        time_cnt[i] = 0;
    }
    long long elapsedTime;
    struct timespec start, end, tmp;
    btree *bt;
    // Make or Read persistent pool

#ifdef PMDK
    TOID(btree)
    tree = TOID_NULL(btree);

    if (file_exists(persistent_path) != 0)
    {
        pool = pmemobj_create(persistent_path, "btree", 16000000000,
                              0666); // make 1GB memory pool
        tree = POBJ_ROOT(pool, btree);
        bt = D_RW(tree);
        bt->constructor();
        bt->base = (void *)((uint64_t)bt - tree.oid.off);
    }
    else
    {
        puts("need recover");
        clock_gettime(CLOCK_MONOTONIC, &start);
        pool = pmemobj_open(persistent_path, "btree");
        clock_gettime(CLOCK_MONOTONIC, &end);
        elapsedTime =
            (end.tv_sec - start.tv_sec) * 1000000000 + (end.tv_nsec - start.tv_nsec);

        if (option == 2)
        {
            cout << "allocator recovery time in us: " << elapsedTime / 1000 << endl;
        }

        clock_gettime(CLOCK_MONOTONIC, &start);
        tree = POBJ_ROOT(pool, btree);
        bt = D_RW(tree);
        bt->constructor();
        bt->base = (void *)((uint64_t)bt - tree.oid.off);
        bt->recover(1);
        clock_gettime(CLOCK_MONOTONIC, &end);
        elapsedTime =
            (end.tv_sec - start.tv_sec) * 1000000000 + (end.tv_nsec - start.tv_nsec);
        if (option == 2)
        {
            cout << "tree recovery time in us: " << elapsedTime / 1000 << endl;
            exit(0);
        }
        cout << "tree height: " << bt->height << endl;
        cout << "traversed node: " << range_node_cnt << endl;
    }
#ifdef A_F
    PMEMoid *pmdklist = (PMEMoid *)malloc(16 * 8800000 * 2);
#pragma omp parallel num_threads(32)
    {
#pragma omp for schedule(static)
        for (int i = 0; i < 8800000 * 2; ++i)
        {
            struct pobj_action actv[1];
            pmdklist[i] = pmemobj_reserve(pool, &actv[0], LEAF_PAGESIZE, 0);
            pmemobj_publish(pool, &actv[0], 1);
            // printf("the key is %lu, the thread is %d\n", keys[i],omp_get_thread_num());
        }
    }
#pragma omp parallel num_threads(n_threads)
    {
#pragma omp for schedule(static)
        for (int i = 0; i < 8800000 * 2; ++i)
        {
            pmemobj_free(&pmdklist[i]);
            // printf("the key is %lu, the thread is %d\n", keys[i],omp_get_thread_num());
        }
    }
#endif

#endif

#ifdef STM
    if (file_exists(persistent_path) != 0) // new
    {

        alloc = new pm_allocator();
        alloc->constructor(persistent_path, 16000000000, 64, LEAF_PAGESIZE);

        bt = (btree *)malloc(sizeof(btree));
        bt->base = (void *)(alloc->master);
        bt->constructor();
    }
    else // recover
    {

        // if (option)
        // {
        //     clock_gettime(CLOCK_MONOTONIC, &start);
        //     alloc = new pm_allocator();
        //     alloc->constructor(persistent_path, 16000000000, 64, LEAF_PAGESIZE, option);
        //     clock_gettime(CLOCK_MONOTONIC, &end);
        //     elapsedTime =
        //         (end.tv_sec - start.tv_sec) * 1000000000 + (end.tv_nsec - start.tv_nsec);
        //     cout << "allocator recovery time in us: " << elapsedTime / 1000 << endl;

        //     clock_gettime(CLOCK_MONOTONIC, &start);
        //     bt = (btree *)malloc(sizeof(btree));
        //     bt->base = (void *)(alloc->master);
        //     bt->constructor();
        //     bt->recover();
        //     clock_gettime(CLOCK_MONOTONIC, &end);
        //     elapsedTime =
        //         (end.tv_sec - start.tv_sec) * 1000000000 + (end.tv_nsec - start.tv_nsec);

        //     cout << "tree recovery time in us: " << elapsedTime / 1000 << endl;
        //     cout << "tree height: " << bt->height << endl;
        //     exit(0);
        // }
        // else
        {
            clock_gettime(CLOCK_MONOTONIC, &start);
            alloc = new pm_allocator();
            alloc->constructor(persistent_path, 16000000000, 64, LEAF_PAGESIZE, false);
            // if(option == 0){
            //     alloc->constructor(persistent_path, 16000000000, 64, LEAF_PAGESIZE, false);
            // }else{
            //     alloc->constructor(persistent_path, 16000000000, 64, LEAF_PAGESIZE, true);
            // }

            clock_gettime(CLOCK_MONOTONIC, &end);
            elapsedTime =
                (end.tv_sec - start.tv_sec) * 1000000000 + (end.tv_nsec - start.tv_nsec);
            if (option == 2)
            {
                cout << "allocator recovery time in us: " << elapsedTime / 1000 << endl;
            }

            bt = (btree *)malloc(sizeof(btree));
            bt->base = (void *)(alloc->master);
            bt->constructor();
            // if (option == 2)
            // {
            // clock_gettime(CLOCK_MONOTONIC, &start);
            // bt->recover(n_threads);
            // clock_gettime(CLOCK_MONOTONIC, &end);
            // elapsedTime =
            //     (end.tv_sec - start.tv_sec) * 1000000000 + (end.tv_nsec - start.tv_nsec);

            // // cout << "tree recovery time in us: " << elapsedTime / 1000 << endl;
            // // cout << "traversed node: " << range_node_cnt << endl;

            // // cout << "tree height: " << bt->height << endl;
            // // exit(0);
            // }
        }
    }

#ifdef A_F
    uint64_t *offsetlist = (uint64_t *)malloc(8 * 8800000 * 2);
#pragma omp parallel num_threads(32)
    {
#pragma omp for schedule(static)
        for (int i = 0; i < 8800000 * 2; ++i)
        {
            offsetlist[i] = alloc->pm_alloc(omp_get_thread_num(), LEAF_PAGESIZE);
            alloc->pm_valid(offsetlist[i]);
        }
    }
#pragma omp parallel num_threads(n_threads)
    {
#pragma omp for schedule(static)
        for (int i = 0; i < 8800000 * 2; ++i)
        {
            alloc->pm_free(offsetlist[i], omp_get_thread_num());
            // printf("the key is %lu, the thread is %d\n", keys[i],omp_get_thread_num());
        }
    }
#endif

#endif
#ifdef NV_MALLOC
    bt = (btree *)malloc(sizeof(btree));
    clock_gettime(CLOCK_MONOTONIC, &start);
    bt->base = nvm_initialize(persistent_path, 1);
    clock_gettime(CLOCK_MONOTONIC, &end);
    elapsedTime =
        (end.tv_sec - start.tv_sec) * 1000000000 + (end.tv_nsec - start.tv_nsec);
    // if (option == 2)
    // {
    //     cout << "allocator recovery time in us: " << elapsedTime / 1000 << endl;
    //     exit(0);
    // }
    bt->constructor();

#ifdef A_F
    char **pointerlist = (char **)malloc(8 * 8800000 * 2);
#pragma omp parallel num_threads(32)
    {
#pragma omp for schedule(static)
        for (int i = 0; i < 8800000 * 2; ++i)
        {
            pointerlist[i] = (char *)nvm_reserve(LEAF_PAGESIZE);
            memset((void *)pointerlist[i], 5, 64);
            nvm_activate((void *)pointerlist[i], NULL, NULL, NULL, NULL);
        }
    }
#pragma omp parallel num_threads(n_threads)
    {
#pragma omp for schedule(static)
        for (int i = 0; i < 8800000 * 2; ++i)
        {
            nvm_free((void *)pointerlist[i], NULL, NULL, NULL, NULL);
            // printf("the key is %lu, the thread is %d\n", keys[i],omp_get_thread_num());
        }
    }
#endif

#endif
    // puts("constructed");
    // printf("base is %lu\n",bt->base);
    // Reading data
    entry_key_t *keys = new entry_key_t[numData];

    ifstream ifs;
    ifs.open("../../sample_input.txt");

    if (!ifs)
    {
        cout << "input loading error!" << endl;
    }

    for (int i = 0; i < numData; ++i)
    {
        ifs >> keys[i];
    }
    ifs.close();

    long half_num_data = numData / 2;

    // Warm-up! Insert half of input size
    if (option < 2)
    {
#pragma omp parallel num_threads(32)
        {
#pragma omp for schedule(static)
            for (int i = 0; i < half_num_data; ++i)
            {
                // printf("the key is %lu, the thread is %d\n", keys[i],omp_get_thread_num());
                bt->btree_insert(keys[i], (char *)keys[i], omp_get_thread_num());
            }
        }
        cout << "xl Warm-up!" << endl;
        printf("%lu node to be allocated\n", split_cnt);
        printf("%lu node leaked\n", fail_cnt);
        // bt->randScounter();
        if (option == 0)
            exit(0);
    }
    clear_cache();
    clock_gettime(CLOCK_MONOTONIC, &start);

#pragma omp parallel num_threads(n_threads)
    {
#pragma omp for schedule(static)
        for (int i = half_num_data; i < numData; ++i)
        {
            // printf("the inserted key is %lu, the thread is %d\n", keys[i],omp_get_thread_num());
            bt->btree_insert(keys[i], (char *)keys[i], omp_get_thread_num());
        }
    }

    clock_gettime(CLOCK_MONOTONIC, &end);
    elapsedTime =
        (end.tv_sec - start.tv_sec) * 1000000000 + (end.tv_nsec - start.tv_nsec);
    // cout << " data num : " <<  numData - half_num_data << endl;
    //  cout << " elapsed time : " <<  elapsedTime << endl;
    printf("avg time spent on insertion is %lu\n", elapsedTime/half_num_data);
    cout << "Concurrent inserting with " << n_threads
         << " threads (Kops) : " << (half_num_data * 1000000) / elapsedTime << endl;
    uint64_t sum = 0;
    for(int i =0;i<n_threads;i++){
        // if(time_cnt[i]==0){
        //     puts("error");
        // }
        sum+=time_cnt[i];
    }
    printf("avg time spent on allocation is %lu\n", sum/half_num_data/16);
    printf("avg time delata is %lu\n", elapsedTime/half_num_data-sum/half_num_data/16);
    printf("the tree height is %d\n", bt->height);
    // bt->check();
//     long data_max = half_num_data * 2;
//     //     clear_cache();
//     clock_gettime(CLOCK_MONOTONIC, &start);

// #pragma omp parallel num_threads(n_threads)
//     {
// #pragma omp for schedule(static)
//         for (int i = half_num_data; i < data_max; ++i)
//         {

//             // printf("the key is %lu, the thread is %d\n", keys[i],omp_get_thread_num());
//             bt->btree_delete(keys[i], omp_get_thread_num());
//         }
//     }

//     clock_gettime(CLOCK_MONOTONIC, &end);
//     elapsedTime =
//         (end.tv_sec - start.tv_sec) * 1000000000 + (end.tv_nsec - start.tv_nsec);
//     // cout << " data num : " <<  numData - half_num_data << endl;
//     //  cout << " elapsed time : " <<  elapsedTime << endl;
//     printf("avg time spent on deletion is %lu\n", elapsedTime/half_num_data);
    
//     cout << "Concurrent delete with " << n_threads
//          << " threads (Kops) : " << (half_num_data * 1000000) / elapsedTime << endl;
//     // bt->check();
//     // printf("the tree height is %d\n", bt->height);
//     sum = 0;
//     for(int i =0;i<n_threads;i++){
//         sum+=time_cnt[i];
//     }
//     printf("avg time spent on deallocation is %lu\n", sum/half_num_data/16);
//     printf("avg time delata is %lu\n", elapsedTime/half_num_data-sum/half_num_data/16);
    // bt->recover();
    delete[] keys;
#ifdef PMDK
    pmemobj_close(pool);
#endif
    return 0;
}
