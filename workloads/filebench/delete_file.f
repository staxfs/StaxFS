set $dir=/dfs
set $nfiles=5000000
set $meandirwidth=1000
set $instances=5
set $nthreads=6
set $runtime=6000
set mode quit alldone

define fileset name=metadataset,path=$dir,entries=$nfiles,dirwidth=$meandirwidth,reuse,trusttree,prealloc

define process name=metadata_process,instances=$instances
{
  thread name=metadata_thread,instances=$nthreads
  {
    flowop deletefile name=deletefile1,filesetname=metadataset
  }
}

echo "Metadata performance test loaded"

run $runtime
