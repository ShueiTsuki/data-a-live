#!/bin/bash
cd /opt/work/tree
sed -n 30,120p iokit/KernelConfigTables.cpp | grep -n -E "IONameMatch|IOClass|#if|#else|#endif"
