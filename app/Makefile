DIR_SRC =  ../
OBJ = api.o cache.o leotp.o udp_leotp.o
EXE = appclient appmid appserver
CC = g++ -std=c++11
FLAGS = -lpthread -g 

all: $(EXE)
 
appclient: ${OBJ} appclient.cpp
	$(CC) -o $@ $^  $(FLAGS)

appmid: ${OBJ} appmid.cpp
	$(CC) -o $@ $^  $(FLAGS)

appserver: ${OBJ} appserver.cpp
	$(CC) -o $@ $^  $(FLAGS)

%.o: ${DIR_SRC}/%.cpp
	$(CC) -c $< $(FLAGS)

clean:
	rm -f $(OBJ) $(EXE)

