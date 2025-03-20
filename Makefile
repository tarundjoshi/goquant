all:
	g++ -std=c++17 ./src/main.cpp ./src/streamer/streamer.cpp ./src/dericlient/dericlient.cpp \
        -I /usr/include \
        -I /usr/local/include \
        -I ./src/ \
        -L /usr/lib -L /usr/local/lib \
        -lsimdjson -lboost_system -lssl -lcrypto -lpthread

clean :
	rm -rf a.out