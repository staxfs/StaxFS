set $dir=/dfs
set $nfiles=5000000
set $meandirwidth=1000
set $instances=5
set $nthreads=6
set $runtime=6000
set mode quit alldone

define fileset name=metadataset,path=$dir,entries=$nfiles,dirwidth=$meandirwidth,prealloc=0

define process name=metadata_process,instances=$instances
{
  thread name=metadata_thread,instances=$nthreads
  {
    flowop createfile name=create_file_op,filesetname=metadataset,fd=1
    flowop closefile name=close_file_op1,fd=1
  }
}

echo "Metadata performance test loaded"

run $runtime
