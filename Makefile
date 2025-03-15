all:
	g++ -std=c++17 websocket_class.cpp \
        -I /usr/include \
        -I /usr/local/include \
        -L /usr/lib -L /usr/local/lib \
        -lsimdjson -lboost_system -lssl -lcrypto -lpthread