all: rtaped

rtaped: src/rtape.c src/record.c src/chaos.c
	gcc -o $@ -Isrc $^
