/**************************************************
 该程序通过标准socket实现简单Http服务器
 运行该服务器可以通过浏览器访问服务器目录下的
 Html文件和jpg图片 完成初步的Http服务器功能
***************************************************/
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/epoll.h>
#include <arpa/inet.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/sendfile.h>
#include <netinet/in.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <assert.h>
#include <time.h>
#include <stdarg.h>
#include <pthread.h>

#define BACKLOG 1000                                    // 同时等待的连接个数
#define MAX_BUF_LEN 1024 
#define FILE_NAME_MAX_SIZE 128
#define MAX_EVENT_NUMBER 1024

struct arg
{
	char store_path[FILE_NAME_MAX_SIZE];
	char buf[MAX_BUF_LEN];
	int sock_fd;
};
char config_file[32] = "../etc/settings";
char log_path[FILE_NAME_MAX_SIZE] = {0};                // 服务器日志存储路径

char * get_options_value(char *options, char * line)
{
	char *temp_str = NULL;
	temp_str = strchr(line, '=');
	strcpy(options, temp_str + 2);
	return options;
}

// 从配置文件 settings 中获得服务器IP地址、服务器端口、存储路径
void load_config(char *server_ip, int *server_port, char *store_path)
{
	FILE *fp = NULL;
    	char line[256];
    	memset(line, 0, sizeof(line));

    	int line_num = 1;
    	fp = fopen(config_file, "r");
    	if (!fp)
    	{
        	printf("line: %d, can't open the config file!\n", __LINE__);
        	return;
    	}
    	char *temp_str1 = NULL;
	char temp_str2[10];
	memset(temp_str2, 0, sizeof(temp_str2));
    	while (feof(fp) != EOF)
    	{
		// fgets 中读取到的内容包含 '\n'
        	if (!fgets(line, 128, fp))
        	{
            		break;
        	}
		// printf("Line: %d, line is %s", __LINE__, line); // for debug
		// 处理 line 中包含的 '\n' 字符
		int i = 0;
		while(line[i] != '\n')
			i++;
		line[i] = '\0';
        	switch(line_num)
        	{
		case 1:
		    strcpy(server_ip, get_options_value(server_ip, line));
		    line_num++;
		    break;
		case 2:
		    temp_str1 = strchr(line, '=');
		    strcpy(temp_str2, temp_str1 + 2);
		    sscanf(temp_str2, "%d", server_port);
		    line_num++;
		    break;
		case 3:
		    strcpy(store_path, get_options_value(store_path, line));
		    line_num++;
		    break;

		case 4:
		    strcpy(log_path, get_options_value(log_path, line));
		    break;
		}
		memset(line, 0, sizeof(line)); // 每次都要进行初始化
    	}
   	 fclose(fp);
}
// 打印日志信息到指定文件中
void LOG(const char* ms, ... )
{
	char wzLog[MAX_BUF_LEN] = {0};
	char buffer[MAX_BUF_LEN] = {0};
	char file_name[FILE_NAME_MAX_SIZE] = {0};
	char log_file_path[MAX_BUF_LEN] = {0};
	
	// 每次进入必须都要初始化
	strncpy(log_file_path, log_path, strlen(log_path));
	
	va_list args;
	va_start(args, ms);
	vsprintf( wzLog,ms, args);
	va_end(args);

	time_t now;
	time(&now);
	struct tm *local;
	local = localtime(&now);
	
	// 每一天每一个小时的日志单独一个文件
	sprintf(file_name,"%04d-%02d-%02d-%02d", local->tm_year+1900, local->tm_mon,
				local->tm_mday, local->tm_hour);
        strcat(log_file_path, file_name);
	
	sprintf(buffer,"%04d-%02d-%02d %02d:%02d:%02d %s", local->tm_year+1900, local->tm_mon,
				local->tm_mday, local->tm_hour, local->tm_min, local->tm_sec,
				wzLog);
    	
	FILE* file = fopen(log_file_path,"a+");
	if (file == NULL) 
	{
	    perror("can't open the log file!\n");
		return;
	}
	fwrite(buffer, 1, strlen(buffer), file);
	fclose(file);
	
	return ;
}
int sendall(int sock, char *buf, int *len)
{
	int bytesent = 0;          // 已经发送字节数
    	int bytesleft = *len;      // 还剩余多少字节
    	int n = 0;
    	while(bytesent < *len)
    	{
        	n = send(sock, buf + bytesent, bytesleft, 0);
        	if (n == -1 && errno != EAGAIN)  // 发送失败
        	{
            		break;
        	}
        	bytesent += n;
        	bytesleft -= n;
    	}
    	*len = bytesent;           // 返回实际发送出去的字节数
    	return n==-1 ? -1 : 0;     // 成功发送返回0 失败-1
}

void wrong_req(int sock) 
{
	char* error_head = "HTTP/1.0 405 Method Not Allowed\r\n"; // 输出405错误
    	int len = strlen(error_head);
    	if (sendall(sock, error_head, &len) == -1)                // 向客户发送
    	{
        	LOG("line: %d, func: %s, sending failed!\n", __LINE__, __FUNCTION__);
        	return;
    	}
	char* error_type = "Content-type: text/plain\r\n";
    	len = strlen(error_type);
    	if (sendall(sock, error_type, &len) == -1)
    	{
        	LOG("line: %d, func: %s, sending failed!\n", __LINE__, __FUNCTION__);
        	return;
    	}

    	char* error_end = "\r\n";
    	len = strlen(error_end);
    	if (sendall(sock, error_end, &len) == -1)
    	{
        	LOG("line: %d, func: %s, sending failed!\n", __LINE__, __FUNCTION__);
        	return;
    	}

    	char* prompt_info = "The method is not yet completed\r\n";
    	len = strlen(prompt_info);
    	if (sendall(sock, prompt_info, &len) == -1)
    	{
        	LOG("line: %d, func: %s, sending failed!\n", __LINE__, __FUNCTION__);
        	return;
    	}
}

int not_exist(char* file_path)
{
	struct stat sbuf;
    	if(stat(file_path, &sbuf) < 0)
    	{
		LOG("line: %d, func: %s, couldn't find this file!\n", __LINE__, __FUNCTION__);
		return -1;
    	}
    	// return (stat(file_path, &dir_info) == -1);
    	// return access(file_path, F_OK);
    	return sbuf.st_size;
}

void file_not_found(char* file_path, int sock)
{
	char* error_head = "HTTP/1.0 404 Not Found\r\n";    // 构造404错误head
    	int len = strlen(error_head);
    	if (sendall(sock, error_head, &len) == -1)          // 向客户端发送
    	{
        	LOG("line: %d, func: %s, sending error!\n", __LINE__, __FUNCTION__);
        	return;
    	}
	char* error_type = "Content-type: text/plain\r\n";
    	len = strlen(error_type);
    	if (sendall(sock, error_type, &len) == -1)
    	{
        	LOG("line: %d, func: %s, sending error!\n", __LINE__, __FUNCTION__);
        	return;
    	}

    	char* error_end = "\r\n";
    	len = strlen(error_end);
    	if (sendall(sock, error_end, &len) == -1)
    	{
        	LOG("line: %d, func: %s, sending error!\n", __LINE__, __FUNCTION__);
        	return;
    	}

    	char prompt_info[50] = "Not found:  ";
    	strcat(prompt_info, file_path);
    	len = strlen(prompt_info);
    	if (sendall(sock, prompt_info, &len) == -1)
    	{
        	LOG("line: %d, func: %s, sending error!\n", __LINE__, __FUNCTION__);
        	return;
    	}
}

void send_header(int send_to, char* content_type)
{
	char* head = "HTTP/1.0 200 OK\r\n";     // 正确的头部信息
    	int len = strlen(head);
    	if (sendall(send_to, head, &len) == -1) // 向连接的客户端发送数据
    	{
        	LOG("line: %d, func: %s, sending error!\n", __LINE__, __FUNCTION__);
        	return;
    	}
    	if (content_type)                       // content_type不为空
    	{
        	char temp_1[125] = "Content-Disposition: attachment\r\nContent-type: "; 
        	strcat(temp_1, content_type);       // 构造content_type
        	strcat(temp_1, "\r\n");
        	len = strlen(temp_1);
        	if (sendall(send_to, temp_1, &len) == -1)
        	{
            		LOG("line: %d, func: %s, sending error!\n", __LINE__, __FUNCTION__);
            		return;
        	}
    	}
}

char* get_file_type(char* arg)
{
    char * temp = NULL;                         // 临时字符串指针
    // strrchr 查找字符在字符数组中最后一次出现的位置
    if ((temp = strrchr(arg, '.')) != NULL)     // 取得后缀
    {
        return temp + 1;
    }
    return "";                                  // 如果请求的文件名中没有. 则返回空串
}
void send_file(char* file_path, int sock, int file_size)
{
    int srcfd;
    char* extension = get_file_type(file_path); // 获得文件后缀名
    char* content_type = "application/octet-stream";
    char read_buf[2*FILE_NAME_MAX_SIZE] = {0};  // 读文件时的字节缓存数组

    LOG("line: %d, func: %s, sending file!\n", __LINE__, __FUNCTION__);
    if (strcmp(extension, "html") == 0)         // 发送内容为html
    {
        content_type = "text/html";
    }

    if (strcmp(extension, "gif") == 0)          // 发送内容为gif
    {
        content_type = "image/gif";
    }

    if (strcmp(extension, "jpg") == 0)         // 发送内容为jpg
    {
        content_type = "image/jpg";
    }
    send_header(sock, content_type);
    
    if ((srcfd = open(file_path, O_RDONLY, 0)) < 0)
    {
	LOG("line: %d, func: %s, open file %s error\n", __LINE__, __FUNCTION__, file_path);
	return;
    }
	
    memset(read_buf, 0, sizeof (read_buf));
    sprintf(read_buf, "Content-Length: %d\r\n", file_size);
    int tmp_len = strlen(read_buf);
    if (sendall(sock, read_buf, &tmp_len) == -1)
    {
        LOG("line: %d, func: %s, sending error!\n", __LINE__, __FUNCTION__);
        return;
    }
    send(sock, "\r\n", 2, 0);                 // 再加一个"\r\n" 不能缺少
	
    sendfile(sock, srcfd, NULL, file_size);   // 零拷贝	
}

void * handle_request(void * my_arg)
{
	struct arg *ptr_arg = (struct arg *)my_arg;
	char method[MAX_BUF_LEN];                    // 保存解析到的方法字段 GET PUT
	char file_path[MAX_BUF_LEN];                 // 保存解析到的请求的文件 

	memset(method, 0, sizeof(method));
	memset(file_path, 0, sizeof(file_path));
	
	strcat(file_path, ptr_arg->store_path);
	// printf("file_path: %s\n", file_path);     // for debug

	if (sscanf(ptr_arg->buf, "%s /%s", method, file_path + strlen(file_path)) != 2)
	{
		close(ptr_arg->sock_fd);
		return;                              // 解析出错返回
	}
	LOG("line: %d, func: %s, handle_cmd:      %s\n", __LINE__, __FUNCTION__, method);
	LOG("line: %d, func: %s,  file_path:      %s\n", __LINE__, __FUNCTION__, file_path);

	if (strcmp(method, "GET") != 0)              // 请求命令格式是否正确
	{
		wrong_req(ptr_arg->sock_fd);
		close(ptr_arg->sock_fd);
		return;
	}
	int file_size = 0;
	if ((file_size = not_exist(file_path)) < 0) // 请求的文件是否存在
	{
		LOG("line: %d, func: %s, file %s not exist!\n", __LINE__, __FUNCTION__, file_path);
		file_not_found(file_path, ptr_arg->sock_fd);
		close(ptr_arg->sock_fd);
		return;
	}
	
	send_file(file_path, ptr_arg->sock_fd, file_size);

	close(ptr_arg->sock_fd);

	return;
}

int make_listenfd(char *server_ip, int server_port)
{
	struct sockaddr_in server_addr;                    // 服务器地址结构体
	int listenfd;                           
    	listenfd = socket(AF_INET, SOCK_STREAM, 0);

    	if (listenfd == -1)                                // 如果返回值为-1 则出错
    	{
        	return -1;
    	}	
	
    	server_addr.sin_family = AF_INET;
    	server_addr.sin_port = htons(server_port);
    	
    	if(!inet_aton(server_ip, &server_addr.sin_addr))   // 换一种安全的方法将IP地址转换为网络字节序
		LOG("line: %d, func: %s, invalid IP address\n", __LINE__, __FUNCTION__);
    	memset(&(server_addr.sin_zero), '\0', 8);
	
    	if (bind(listenfd, (struct sockaddr *)&server_addr, sizeof(server_addr)) == -1)
    	{
	    	LOG("line: %d, func: %s, bind error!\n", __LINE__, __FUNCTION__);
            	return -1;
   	 }

    	if (listen(listenfd, BACKLOG) == -1 )
    	{
		LOG("line: %d, func: %s, listen error!\n", __LINE__, __FUNCTION__);
        	return -1;
   	 }

    	return listenfd;
} 

int setnonblocking(int fd)
{
	int old_option = fcntl(fd, F_GETFL);              // 获得监听套接字的文件描述符的文件状态
	int new_option = old_option | O_NONBLOCK;
	fcntl(fd, F_SETFL, new_option);
	return old_option;
}
void addfd(int epollfd, int fd, bool oneshot)
{
	struct epoll_event event;
	event.data.fd = fd;
	event.events = EPOLLIN | EPOLLET;
	
	if (oneshot)
	{
		event.events |= EPOLLONESHOT;
	}
	
	int ret = epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);
	if (ret < 0) 
	{
		LOG("line: %d, func: %s, errno is: %d\n", __LINE__, __FUNCTION__, errno);
		return;
	}
	setnonblocking(fd);
	
}
// 重置 fd 上的事件
void reset_oneshot( int epollfd, int fd)
{
	struct epoll_event event;
	event.data.fd = fd;
	event.events = EPOLLIN | EPOLLET | EPOLLONESHOT;
	epoll_ctl(epollfd, EPOLL_CTL_MOD, fd, &event);
}
void et(struct epoll_event *events, int number, int epollfd, int listenfd,
        char * store_path)
{
	int i = 0;
	char buf[MAX_BUF_LEN];
	struct epoll_event ev;
	for (i = 0; i < number; i++)
	{
		int sockfd = events[i].data.fd;
		if (sockfd == listenfd)
		{
			struct sockaddr_in client_address;
			socklen_t client_addrlength = sizeof(client_address);
			int connfd = accept(listenfd, (struct sockaddr*)&client_address, & client_addrlength);
			if (connfd < 0)
			{
				LOG("line: %d, func: %s, errno is: %d\n", __LINE__, __FUNCTION__, errno);
				break;
			}
			// 对每个非监听文件描述符都注册 EPOLLONESHOT 事件
			addfd(epollfd, connfd, true);
		}
		else if(events[i].events & EPOLLIN)
		{
			int n = 0;
			int nread = 0;
			memset(buf, '\0', MAX_BUF_LEN);
	
			// recv() 调用通常用在一个 connected socket 上
			// 当 socket 上没有消息到达时,该调用会一直阻塞;
			// 当 socket 是 nonblocking 时, 返回-1，并且 errno 会被设置成 EAGAIN 或 EWOULDBLOCK，必须要进行检查
			while(1)
			{
				nread = recv(sockfd, buf + n, MAX_BUF_LEN - 1, 0);
				if (nread == 0)
				{
					close(sockfd);
					LOG("line: %d, func: %s, client closed the connection\n", __LINE__, __FUNCTION__);
					break;
				}
				else if (nread < 0)
				{
					if ((errno == EAGAIN) || (errno == EWOULDBLOCK))
					{
						reset_oneshot(epollfd, sockfd);
						LOG("line: %d, func: %s, read later\n", __LINE__, __FUNCTION__);
						break;
					}
				}
				n += nread;
			}
			ev.data.fd = sockfd;
			// 设置用于注册的写操作事件
			ev.events = EPOLLOUT | EPOLLET;
			// 修改 sockfd 上要处理的事件为 EPOLLOUT
		    if (epoll_ctl(epollfd, EPOLL_CTL_MOD, sockfd, &ev) == -1) 
			{
				LOG("line: %d, func: %s, epoll_ctl: mod\n", __LINE__, __FUNCTION__);
			}
		}
		else if (events[i].events & EPOLLOUT)
		{
			pthread_attr_t attr;
                    	pthread_t threadId;
                
		    	// 初始化属性值，均设为默认值 
		    	pthread_attr_init(&attr); 
		    	pthread_attr_setscope(&attr, PTHREAD_SCOPE_SYSTEM); 
		    	// 设置线程为分离属性 
		    	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

		    	struct arg my_arg;
		    	memset(my_arg.store_path, 0, sizeof(my_arg.store_path));
		    	memset(my_arg.buf, 0, sizeof(my_arg.buf));
		    	my_arg.sock_fd = 0;

		    	strcpy(my_arg.store_path, store_path);
		    	strcpy(my_arg.buf, buf);
		    	my_arg.sock_fd = sockfd;

		    	if(pthread_create(&threadId,&attr, handle_request,(void *)&my_arg))
		    	{ 
				perror("pthread_create error!"); 
				exit(-1); 
		    	} 
		}
	}
}
int main(int argc, char * argv[])
{
	char server_ip[FILE_NAME_MAX_SIZE];                 // 服务器IP地址
	memset(server_ip, 0, sizeof(server_ip));
	
	int server_port = 0;                                // 服务器端口值

    	char store_path[FILE_NAME_MAX_SIZE];
    	memset(store_path, 0, sizeof(store_path));          // 服务器文件存储路径
	
	load_config(server_ip, &server_port, store_path);
	LOG("line: %d, func: %s, my web server started...\n", __LINE__, __FUNCTION__);
	
    	int listenfd;                                       // 服务器的socket
	listenfd = make_listenfd(server_ip, server_port);   // 创建服务器端的socket

    	if (listenfd == -1)                                 // 创建socket出错
    	{
		LOG("line: %d, func: %s, server exception!\n", __LINE__, __FUNCTION__);
        	exit(2);
    	}

     	struct epoll_event events[MAX_EVENT_NUMBER];
     
     	int epollfd = epoll_create1(EPOLL_CLOEXEC);         // 创建一个 epoll 实例
     	if (epollfd < 0)
     	{
         	LOG("line: %d, func: %s, create epoll instance failed!\n", __LINE__, __FUNCTION__);
	 	exit(-1);
     	}
	
     	addfd( epollfd, listenfd, false);
     	/**
     	1. 应用程序注册感兴趣的事件到内核;
     	2. 内核在某种条件下，将事件通知应用程序;
     	3. 应用程序收到事件后，根据事件类型做相应的逻辑处理.
     	**/
     	while (1)
     	{
	     	int ret = epoll_wait( epollfd, events, MAX_EVENT_NUMBER, 0);
	     	if (ret < 0)
	     	{
		     	LOG("line: %d, func: %s, epoll failure\n", __LINE__, __FUNCTION__);
		     	break;
	     	}
		et(events, ret, epollfd, listenfd, store_path); 
	}
     	close(epollfd);
     	return 0;
}
