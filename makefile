output: main.o resource.o
	gcc *.o -mwindows -lwinmm -lmsimg32

main.o: main.c menus.h
	gcc -c main.c

resource.o: menus.h resource.rc 
	windres resource.rc -o resource.o

clean:
	rm -f *.o output

sprite:
	rm -f resource.o
	windres resource.rc -o resource.o

