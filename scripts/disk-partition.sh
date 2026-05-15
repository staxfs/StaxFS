#!/bin/bash

if [ $# -lt 1 ]; then
    echo "Give me device name in parameter 1!"
    exit
fi
if [ $# -lt 2 ]; then
    echo "Give me partition size in parameter 2! Note that all partitions will be of the same size."
    exit
fi
echo "Making 4 partitions on /dev/$1 of size $2."
read -p "Original partitions will be wiped out! Are you sure? [Y/N]" -n 1 -r
echo    # (optional) move to a new line
if [[ ! $REPLY =~ ^[Yy]$ ]]
then
    [[ "$0" = "$BASH_SOURCE" ]] && exit 1 || return 1 # handle exits from shell or function but don't exit interactive shell
fi
sed -e 's/\s*\([\+0-9a-zA-Z]*\).*/\1/' << EOF | sudo fdisk /dev/$1
  d # remove existing partition
    # default = last partition
  d # remove existing partition
    # default = last partition
  d # remove existing partition
    # default = last partition
  d # remove last partition
  n # new partition
  p # primary partition
  1 # partition number 1
    # default - start at beginning of disk 
  +$2
  n # new partition
  p # primary partition
  2 # partion number 2
    # default, start immediately after preceding partition
  +$2
  n # new partition
  p # primary partition
  3 # partion number 3
    # default, start immediately after preceding partition
  +$2
  n # new partition
  p # primary partition
  4 # partion number 4
    # default, start immediately after preceding partition
  +$2
  w # write the partition table
  q # and we're done
EOF