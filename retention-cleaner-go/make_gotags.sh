#!/bin/bash -x
find $PWD -name '*.go' | gotags -f gotags -R -L -
find $PWD -name '*.go' > cscope.files
cscope -bvq
