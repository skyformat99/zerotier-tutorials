#include <stdio.h>
#include "pthread.h"

#include "ZeroTierOne.h"

#include "Service.h"

void *main_zt_thread(void *thread_id)
{
	printf("main_zt_thread\n");
	Service *srv = new Service();
	srv->run();
	return NULL;
}

int main()
{
	pthread_t service_thread;
	int err = pthread_create(&service_thread, NULL, main_zt_thread, NULL);
	pthread_join(service_thread, NULL);
	return 1;
}