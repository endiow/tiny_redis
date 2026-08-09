all : redis client

redis : server.o hashtable.o avl.o zset.o heap.o thread_pool.o
	g++ server.o hashtable.o avl.o zset.o heap.o thread_pool.o -o redis -lpthread

client: 
	g++ client.cpp -o client

server.o : server.cpp
	g++ server.cpp -c 

hashtable.o : hashtable.cpp
	g++ hashtable.cpp -c 

avl.o : avl.cpp 
	g++ avl.cpp -c

zset.o : zset.cpp 
	g++ zset.cpp -c

heap.o : heap.cpp
	g++ heap.cpp -c

thread_pool.o : thread_pool.cpp
	g++ thread_pool.cpp -c 

clean:
	rm redis && rm client && rm *.o
