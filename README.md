stallocator_dis.h is the source code of our STM-manager
fast_s.h is our customized version of FAST&FAIR for test

Confirm you have already configured your PM device well (including installed PMDK), our default PM path can be find in the *.sh files

Running test_allocator.sh can test the performance of the memory managers. More test configuration can be found in test_allocator.cpp.

Before testing the implementations in B+-tree, workloads should be generated first (workloads generated for cc-tree can be used without blocking). The workload path and test information are configured in test_tree.cpp. test_tree.sh is used for testing the insertion and deletion performance of different memory managers. test_recover.sh tests the recovery performance of both the concurrent strategy and the single-threaded strategy.
