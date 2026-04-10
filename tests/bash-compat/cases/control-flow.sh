#!/usr/bin/env bash
sum=0

for i in 1 2 3; do
    sum=$(( sum + i ))
done

if [ "$sum" -eq 6 ]; then
    echo "sum=$sum"
else
    echo "bad=$sum"
fi

i=1
while [ "$i" -lt 4 ]; do
    echo "loop:$i"
    i=$(( i + 1 ))
done
