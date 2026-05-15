set $dir=/dfs
set $nfiles=5000000
set $meandirwidth=1000
set $instances=5
set $nthreads=6
set $count=500000
set $runtime=6000
set mode quit alldone

set $fileidx = randvar(type=tabular, randtable={
{23, 1, 28},
{15, 29, 333},
{15, 334, 3719},
{12, 3720, 24595},
{10, 24596, 115590},
{8, 115591, 391916},
{7, 391917, 1127010},
{5, 1127011, 2380421},
{3, 2380422, 3718181},
{2, 3718182, 5000000}
})

define fileset name=metadataset,path=$dir,entries=$nfiles,dirwidth=$meandirwidth,reuse,trusttree,prealloc

define process name=metadata_process,instances=$instances
{
  thread name=metadata_thread,instances=$nthreads
  {
    flowop statfile name=statfile1,filesetname=metadataset,indexed=$fileidx,iters=$count
    flowop finishoncount name=finishoncount,value=1
  }
}

echo "Metadata performance test loaded"

run $runtime
