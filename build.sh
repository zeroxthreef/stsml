#!/bin/sh
xxd -i -a lib/SimpleTinyScript/stdlib.sts > stdlib.h
# HIGHLY recommend leaving the ub and address sanitizers enabled. The code quality for just about everything in this project down to the scripting language itself is incredibly sketchy
cc -fsanitize=undefined -fsanitize=address -Wall -g -o stsml src/main.c src/parser.c src/util.c lib/SimpleTinyScript/cli.c -lonion -lhiredis -lpthread -lm -DNO_CLI_MAIN=1 -DCOMPILING=1 -DSTS_GOTO_JIT