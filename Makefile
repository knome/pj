
pj: pj.c Makefile
	gcc -std=c99 -Wfatal-errors -Werror -g -O2 -o pj pj.c

.dummy: install
install:
	cp pj ~/bin/
