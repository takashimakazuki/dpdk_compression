#!/bin/bash

ninja -C ../src/build && \
../src/build/doca_comm_compression_total --json option.json
