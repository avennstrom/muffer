

muffer: main.c
	gcc -g -o $@ $^ -lraylib -lvorbis -lvorbisenc -lm -logg -lcurl