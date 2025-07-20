#!/bin/bash
find src -iname *.h -o -iname *.c -o -iname *.cpp -o -iname *.hpp | xargs clang-format -i
# find include -iname *.h -o -iname *.c -o -iname *.cpp -o -iname *.hpp | xargs clang-format -i

