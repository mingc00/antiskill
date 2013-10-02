NAME=antiskill
CC=gcc
OPT=-o $(NAME) -O2
OBJ=antiskill.c

all:
	$(CC) $(OBJ) $(OPT)
clean:
	rm -f $(NAME)
