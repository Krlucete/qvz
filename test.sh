#!/bin/bash

set -v

make debug
bin/qvz -c 1 -f 0.5 -s test.in test.q > write
bin/qvz -x test.q test.dec > read
diff fref.txt test.dec
