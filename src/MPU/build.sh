#!/bin/bash
set -e
cd "$(dirname "$0")"

sudo pkill -INT -x main

g++ -std=c++17 -O2 \
  main.cpp -o main \
  $(pkg-config --cflags --libs opencv4) \
  -I/usr/local/cuda/include \
  -L/usr/local/cuda/lib64 \
  -lnvinfer -lnvinfer_plugin -lcudart -lcurl -pthread