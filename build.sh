#!/bin/bash

 gcc -o player core_test.c core.c -lavformat -lavcodec -lswscale -lavutil -lswresample -lz `sdl2-config --cflags --libs`
