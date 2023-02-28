
path_nv="/mnt/ywpmem0"
path_stm="/mnt/ywpmem0/data0"
path_pmdk="/mnt/ywpmem0/data1"
output="result.txt"
thread_num=32
ops_num=100000
# if [ -f "$path0" ]; then
#     echo "file exists"
# else
#     echo "generating empty files"
#     dd if=/dev/zero of=$path0 bs=1M count=16384
#     # dd if=/dev/zero of=/mnt/ywpmem0/data0 bs=1M count=1024
#     echo "file generated"
# fi
# export LD_PRELOAD=/usr/lib/x86_64-linux-gnu/libjemalloc.so.2

# source /opt/intel/oneapi/setvars.sh
# g++ test_allocator.cpp -o test_makalu -pthread -DMAKALU -L. -lmakalu -O3
# export LD_LIBRARY_PATH=$LD_LIBRARY_PATH:$(pwd):
g++ test_allocator.cpp -o stm -L. -lnvmmalloc -pthread -DSTM -w -O3
g++ test_allocator.cpp -o nv -L. -lnvmmalloc -pthread -DNVMALLOC -w -O3
g++ test_allocator.cpp -o pmdk -pthread -DPMDK -w -lpmemobj -O3


# echo "--------PMDK-----------"
# numactl --cpubind=0 --membind=0 ./test_pmdk
rm $output
echo "      Allocation"
echo "      Allocation" >> result.txt
for((t=1;t<=1;t=t*2))
do
echo "thread number : $t-----" >> result.txt
echo "       STM-manager" >> result.txt
# numactl --cpubind=0 --membind=0 vtune -collect memory-access -knob sampling-interval=0.01 ./stm -n $ops_num -t $t -p $path_stm -o 0 >> $output

numactl --cpubind=0 --membind=0 perf record -e instructions -a -- ./stm -n $ops_num -t $t -p $path_stm -o 0 >> $output
# echo "       PMDK" >> result.txt
# if [ -f "$path_pmdk" ]; then
#     rm $path_pmdk
# fi
# # numactl --cpubind=0 --membind=0 vtune -collect memory-access -knob sampling-interval=0.01 ./pmdk -n $ops_num -t $t -p $path_pmdk -o 0 >> $output
# numactl --cpubind=0 --membind=0 perf stat -e cpu/event=0xae,umask=0x01,name=clwb/ ./pmdk -n $ops_num -t $t -p $path_pmdk -o 0 >> $output 
# echo "       nvm_malloc" >> result.txt
# # numactl --cpubind=0 --membind=0 vtune -collect memory-access -knob sampling-interval=0.01 ./nv -n $ops_num -t $t -p $path_nv -o 0 >> $output
# numactl --cpubind=0 --membind=0 perf stat -e cpu/event=0xae,umask=0x01,name=clwb/ ./nv -n $ops_num -t $t -p $path_nv -o 0 >> $output

# echo "       STM-manager" >> result.txt
# numactl --cpubind=0 --membind=0 ./stm -n $ops_num -t $t -p $path_stm -o 0 >> $output
# echo "       PMDK" >> result.txt
# if [ -f "$path_pmdk" ]; then
#     rm $path_pmdk
# fi
# numactl --cpubind=0 --membind=0 ./pmdk -n $ops_num -t $t -p $path_pmdk -o 0 >> $output
# echo "       nvm_malloc" >> result.txt
# numactl --cpubind=0 --membind=0 ./nv -n $ops_num -t $t -p $path_nv -o 0 >> $output

# echo "---------------------------------------">> $output
done

echo "      Free"
echo "      Free" >> result.txt
for((t=64;t<=64;t=t*2))
do
# echo "thread number : $t-----" >> result.txt
# echo "       STM-manager" >> result.txt
# numactl --cpubind=0 --membind=0 ./stm -n $ops_num -t $t -p $path_stm -o 1 >> $output
# echo "       PMDK" >> result.txt
# if [ -f "$path_pmdk" ]; then
#     rm $path_pmdk
# fi
# numactl --cpubind=0 --membind=0 ./pmdk -n $ops_num -t $t -p $path_pmdk -o 1 >> $output
# echo "       nvm_malloc" >> result.txt
# numactl --cpubind=0 --membind=0 ./nv -n $ops_num -t $t -p $path_nv -o 1 >> $output

# echo "---------------------------------------">> $output
done


echo "      Allocate after Free"
echo "      Allocate after Free" >> result.txt
for((t=64;t<=64;t=t*2))
do
# echo "thread number : $t-----" >> result.txt
# echo "       STM-manager" >> result.txt
# numactl --cpubind=0 --membind=0 ./stm -n $ops_num -t $t -p $path_stm -o 2 >> $output
# echo "       PMDK" >> result.txt
# if [ -f "$path_pmdk" ]; then
#     rm $path_pmdk
# fi
# numactl --cpubind=0 --membind=0 ./pmdk -n $ops_num -t $t -p $path_pmdk -o 2 >> $output
# echo "       nvm_malloc" >> result.txt
# numactl --cpubind=0 --membind=0 ./nv -n $ops_num -t $t -p $path_nv -o 2 >> $output

# echo "---------------------------------------">> $output
done
rm r0* -rf
rm stm
rm nv
rm pmdk
# numactl --cpubind=0 --membind=0 ./test_makalu