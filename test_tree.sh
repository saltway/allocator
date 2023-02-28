number=200000000
thread=1
optimizor=-O3

path0="/mnt/ywpmem0/data3"
path1="/mnt/ywpmem0/"
path2="/mnt/ywpmem0/data0"
rm $path0
# rm $path1
# rm $path2
rm /mnt/ywpmem0/meta
rm /mnt/ywpmem0/backing
# if [ -f "$path2" ]; then
#     echo "file exists"
# else
#     echo "generating empty files"
#     dd if=/dev/zero of=$path2 bs=1M count=16384
#     # dd if=/dev/zero of=/mnt/ywpmem0/data0 bs=1M count=1024
#     echo "file generated"
# fi
# export LD_LIBRARY_PATH=$LD_LIBRARY_PATH:$(pwd):
# export LD_PRELOAD=/usr/lib/x86_64-linux-gnu/libjemalloc.so.2
echo "test begin"
echo "compiling"
g++  test_tree.cpp -o fast_stm -std=c++11 -pthread -lpmemobj $optimizor -w -mrtm -fopenmp -lpmem -DSHIFT -DSTM 
g++  test_tree.cpp -o fast_pmdk -std=c++11 -pthread -lpmemobj $optimizor -w -mrtm -fopenmp -lpmem -DSHIFT -DPMDK 
g++  test_tree.cpp -o fast_nv -std=c++11 -pthread -lpmemobj $optimizor -w -mrtm -fopenmp -lpmem -DSHIFT -DNV_MALLOC -L. -lnvmmalloc
#  -DA_F
# g++  test_tree.cpp -o fast_p -std=c++11 -pthread -lpmemobj $optimizor -w -mrtm -fopenmp -lpmem -DSHIFT -DPERSISTENT
echo "compiled"

# wrmsr -a 0x1a4 15
for((t=2;t<=64;t=t*2))
do
 echo "thread number: $t"
 rm $path2
 if [ -f "$path2" ]; then
    echo "file exists"
else
    echo "generating empty files"
    dd if=/dev/zero of=$path2 bs=1M count=16384
    # dd if=/dev/zero of=/mnt/ywpmem0/data0 bs=1M count=1024
    echo "file generated"
fi
  echo "stm:"
  numactl --cpubind=0 --membind=0 ./fast_stm -n $number -t $t -p $path2 -o 1
  # -n 200 -t 1 -p /mnt/ywpmem0/data0 -o 2
  echo "nv_malloc:"
  numactl --cpubind=0 --membind=0 ./fast_nv -n $number -t $t -p $path1 -o 1
  echo "pmdk:"
  numactl --cpubind=0 --membind=0 ./fast_pmdk -n $number -t $t -p $path0 -o 1
  rm $path0

  
  # echo "persistent fast:"
  # numactl --cpubind=0 --membind=0 ./fast_p -n $number -t $t -p $path1 -o 1
  # -n 100 -t 1 -p /mnt/ywpmem0/data0 -o 1
  
# rm $path0
# rm $path1
echo "----------------------------------------------------"
done
rm fast_nv
rm fast_pmdk
rm fast_stm
# wrmsr -a 0x1a4 0
# rm ztree
echo "test finished"