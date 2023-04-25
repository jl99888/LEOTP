DIR_SRC =  ../
APP = appclient appmid appserver
all: $(APP)
objs = api.o cache.o leotp.o udp_leotp.o
appclient: ${objs} appclient.cpp
	g++ appclient.cpp  ${objs} -o appclient -lpthread -g -std=c++11
appmid: ${objs} appmid.cpp
	g++ appmid.cpp  ${objs} -o appmid -lpthread -g -std=c++11
appserver: ${objs} appserver.cpp
	g++ appserver.cpp  ${objs} -o appserver -lpthread -g -std=c++11
%.o: ${DIR_SRC}/%.cpp
	g++ -c $< -lpthread -g -std=c++11
clean:
	rm -f *.o
	rm -f appclient
	rm -f appmid
	rm -f appserver

