#!/bin/bash
cd /opt/work/tree
git diff --quiet || echo DIRTY
grep -rn -E 'ml_io_map(_wcomb)?\(0x[0-9a-fA-F]+' --include=*.c --include=*.cpp --include=*.h . | grep -v -E '^\./(EXTERNAL_HEADERS|tools)/' | head -40
