

muffer: main.c
	gcc -o $@ $^ -lraylib -lvorbis -lvorbisenc -lm -logg -lcurl