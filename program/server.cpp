#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netdb.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <string>
#include <map>
#include <queue>
#include <vector>
#include <sys/epoll.h>

#include "Util.h"
#include "LockedList.h"
#include "Connection.h"

using std::map;
using std::string;
using std::queue;
using std::vector;

LockedList<TaskInfo> g_TaskList;
static bool g_exitFlag;
map<int, Connection> g_connections;

int CreateTcpSocket(unsigned short port)
{
	int fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0)
	{
		perror("cannot create socket");
		return -1;
	}
	const int on = 1;
	if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on)) != 0) {
		perror("setsockopt failed");
		return -2;
	}

	struct sockaddr_in addr;
	memset((void *)&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_ANY);
	addr.sin_port = htons(port);
	if (0 != bind(fd, (struct sockaddr *)&addr, sizeof(addr)))
	{
		perror("bind failed");
		return -3;
	}

	if (0 != listen(fd, 1024))
	{
		perror("listen failed");
		return -4;
	}
	return fd;
}

int ProcessTask(const TaskInfo& task, string &response)
{
	int64_t tcStart = GetTickCount(1);
	int64_t iterateTimes = task.iterateTimes;
	const int pace = 100000000;
	int tmpPace = pace;
	for (int64_t i = 0; i < iterateTimes; i++)
	{
		if (--tmpPace == 0)
		{
			int64_t tc = GetTickCount(1);
			printf("num_remain = %ld, already cost %ldms\n", iterateTimes - i, tc - tcStart);
			tmpPace = pace;
		}
	}
	response = "request_id=" + task.reqId + "&send_time=" + std::to_string(task.sendTime);
	return 0;
}

void *WorkerThreadProc(void *lp)
{
	while (!g_exitFlag)
	{
		TaskInfo task = g_TaskList.pop();
		string response;
		int ret = ProcessTask(task, response);
		if (ret != 0)
		{
			printf("ProcessTask failed reqest_id=%s\n", task.reqId.c_str());
			continue;
		}
		int nbytes = write(task.sock, response.c_str(), response.length());
	}
}

int main(int argc, char *argv[])
{
	if (argc != 2)
	{
		printf("invalid parameter\n");
		printf("Usage: ./server [workerThreadCnt]\n");
		return -1;
	}

	int workerThreadCnt = atoi(argv[1]);

	g_exitFlag = false;
	int listenFd = CreateTcpSocket(DEFAULT_PORT);
	if (listenFd < 0)
	{
		printf("create socket failed ret=%d\n", listenFd);
		return -1;
	}

	for (int i = 0; i < workerThreadCnt; i++)
	{
		pthread_t tid;
		int ret = pthread_create(&tid, NULL, WorkerThreadProc, NULL);
		if (0 != ret)
		{
			perror("create connect thread fail");
			return -2;
		}
	}

	struct epoll_event ev, events[MAX_EVENTS];
	int epollFd = epoll_create1(0);

	ev.events = EPOLLIN;
	ev.data.fd = listenFd;
	epoll_ctl(epollFd, EPOLL_CTL_ADD, listenFd, &ev);

	while (!g_exitFlag)
	{
		int activeFdCnt = epoll_wait(epollFd, events, MAX_EVENTS, -1);
		for (int i = 0; i < activeFdCnt; i++)
		{
			if (events[i].data.fd == listenFd)
			{
				struct sockaddr_in clientAddr;
				socklen_t addrLen = sizeof(clientAddr);
				int connectSocket = accept(listenFd, (struct sockaddr *)&clientAddr, &addrLen);
				if (connectSocket == -1)
				{
					if (errno != EAGAIN && errno != EWOULDBLOCK)
					{
						perror("accept error");
					}
					printf("conncetion error\n");
					continue;
				}
				printf("accept new connection\n");
				SetNonBlocking(connectSocket);
				ev.events = EPOLLIN | EPOLLET;
				ev.data.fd = connectSocket;
				epoll_ctl(epollFd, EPOLL_CTL_ADD, connectSocket, &ev);
				Connection conn(connectSocket);
				g_connections[connectSocket] = conn;

			}
			else
			{
				map<int, Connection>::iterator iter = g_connections.find(events[i].data.fd);
				if (iter == g_connections.end())
				{
					printf("connection not found, sock=%d\n", events[i].data.fd);
					return -1;
				}
				vector<TaskInfo> taskArr;
				int taskCnt = (iter->second).read(taskArr);
				
				for (int i = 0; i < taskCnt; i++)
				{
					g_TaskList.push(taskArr[i]);
				}
				//if (!flag) // EOF, connection closed by client
				//{
				//	epoll_ctl(epollFd, EPOLL_CTL_DEL, events[i].data.fd, &events[i]);
				//	close(events[i].data.fd);
				//}
			}
		}
	}

	printf("program exit normally\n");

	return 0;
}
