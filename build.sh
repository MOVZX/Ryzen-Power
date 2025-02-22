#!/usr/bin/env bash

gcc -O2 -o ryzen ryzen.c -lm
gcc -O2 -o cpuf cpuf.c -lm
gcc -O2 -o sens sens.c -lm
gcc -O2 -o powerusage powerusage.c -lm
