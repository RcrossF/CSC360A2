.phony all:
all: acs

acs: ACS.c
	gcc-9 -Wall ACS.c -lpthread -o ACS -g


.PHONY clean:
clean:
	-rm -rf *.o *.exe
