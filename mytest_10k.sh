#!/bin/bash
ulimit -n 65536
./test_presure/webbench-1.5/webbench -c 10000 -t 10 http://192.168.80.130:4422/