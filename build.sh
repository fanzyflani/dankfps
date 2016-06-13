#!/bin/sh
cc -O1 -g -o dankfps-server -DSERVER main.c -lm
cc -O1 -g -o dankfps main.c `sdl2-config --cflags --libs` -lepoxy -lm

