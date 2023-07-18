#!/bin/bash

ninja -C ../src/build_x64 && \
../src/build_x64/doca_comm_compression --json option_host.json

