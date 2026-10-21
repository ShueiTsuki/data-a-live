#!/bin/bash
curl -s -m 30 https://www.swift.org/api/v1/install/releases.json | python3 -c '
import sys, json
d = json.load(sys.stdin)
r = d[-1]
print(r["name"], r.get("date"))
for p in r["platforms"]:
    if "24.04" in p.get("name", "") or "2404" in json.dumps(p):
        print(p)
'
