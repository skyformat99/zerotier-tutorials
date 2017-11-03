CXX=clang++
CFLAGS=-Ofast --std=c++11
INCLUDES=-Izto/node -Izto/osdep -Izto/include -Izto/ext/json

clean:
	rm -rf service

all:
	$(CXX) $(CFLAGS) $(INCLUDES) Service.cpp main.cpp -L. -lzerotiercore -o service