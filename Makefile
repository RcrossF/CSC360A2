.phony all:
all: acs

acs: ACS.c
	gcc -Wall ACS.c -lpthread -o ACS -g


.PHONY clean:
clean:
	-rm -rf *.o *.exe
