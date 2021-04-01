#!/bin/sh
xxd -i -a lib/SimpleTinyScript/stdlib.sts > stdlib.h
cc -Wall -g -o stsml src/main.c src/parser.c src/util.c lib/SimpleTinyScript/cli.c -lonion -lhiredis -lpthread -lm -DNO_CLI_MAIN=1 -DCOMPILING=1 -DSTS_GOTO_JIT