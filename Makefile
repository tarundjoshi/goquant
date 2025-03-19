all:
	g++ -std=c++17 websocket_class.cpp streamer.cpp dericlient.cpp \
        -I /usr/include \
        -I /usr/local/include \
        -I ./ \
        -I ./quill/include \
        -L /usr/lib -L /usr/local/lib \
        -lsimdjson -lboost_system -lssl -lcrypto -lpthread