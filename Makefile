NAME := $(shell basename $(PWD))
export MODULE := M2
all: $(NAME)-64.so 
CFLAGS += -U_FORTIFY_SOURCE

include ../Makefile
