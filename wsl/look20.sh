#!/bin/bash
cd /opt/work/tree
grep -rn -E 'xiaomi,|sm8550-cpu|kryo470|fuxi-ufs|sweet-ufs|sweet-fbcon|fuxi-gpu-memory|"/fuxi|"/sweet' --include=*.c --include=*.cpp --include=*.h . | grep -v -E '^\./(EXTERNAL_HEADERS|tools)/' | head -30
