server: main.cpp ./threadpool/threadpool.h ./http/http_conn.cpp ./http/http_conn.h ./lock/locker.h ./log/log.cpp ./log/log.h ./log/block_queue.h ./CGImysql/sql_connection_pool.cpp ./CGImysql/sql_connection_pool.h ./io_uring/io_uring.h ./queue/queue.h
	g++ -o server main.cpp ./threadpool/threadpool.h ./http/http_conn.cpp ./http/http_conn.h ./lock/locker.h ./log/log.cpp ./log/log.h ./CGImysql/sql_connection_pool.cpp ./CGImysql/sql_connection_pool.h ./io_uring/io_uring.h ./queue/queue.h -lpthread -lmysqlclient -Wformat-truncation=0 -luring -g -O2

clean:
	rm -r server
