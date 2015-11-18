#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdio.h>
#include <pthread.h>
#include <signal.h>

#ifdef __XENO__
#include <rtdm/can.h>
#else
#include <time.h>
#include <net/if.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <linux/can.h>
#include <linux/can/raw.h>
#endif

#define REALTIME
#define CLOCK CLOCK_MONOTONIC

int can_socket(const char *ifname) {
	int fd, st, ret = 0;
	struct sockaddr_can addr;
	struct ifreq ifr;
	
	fd = socket(PF_CAN, SOCK_RAW, CAN_RAW);
	if(fd < 0)
	{
		ret = -1;
		perror("Error while opening socket");
		goto quit;
	}
	
	strcpy(ifr.ifr_name, ifname);
	st = ioctl(fd, SIOCGIFINDEX, &ifr);
	if(st < 0)
	{
		ret = -2;
		perror("Error SIOCGIFINDEX ioctl to CAN socket");
		goto close;
	}
	
	memset(&addr, 0, sizeof(addr));
	addr.can_family  = AF_CAN;
	addr.can_ifindex = ifr.ifr_ifindex;
	
	printf("%s at index %d\n", ifname, ifr.ifr_ifindex);
	
	st = bind(fd, (struct sockaddr *) &addr, sizeof(addr));
	if(st < 0) 
	{
		ret = -3;
		perror("Error in socket bind");
		goto close;
	}
	
	return fd;
	
close:
	close(fd);
quit:
	return ret;
}

int done = 0;

void sighandler(int signo) {
	done = 1;
}

struct cookie {
	int fds, fdr;
	struct timespec *lts;
};

#define NS_SEC 1000000000l
#define FREQ   100

void *send_main(void *data) {
	struct cookie *cookie = (struct cookie*) data;
	int st, fd = cookie->fds;
	struct timespec ts, *lts = cookie->lts;
	struct can_frame frame;
	
#ifdef REALTIME
	struct sched_param param;
	param.sched_priority = 80;
	pthread_setschedparam(pthread_self(), SCHED_FIFO, &param);
#endif
	
	frame.can_id = 0xf;
	frame.can_dlc = 1;
	frame.data[0] = 0xff;
	
	ts.tv_sec = 0;
	ts.tv_nsec = NS_SEC/FREQ;
	
	while(!done) {
		clock_gettime(CLOCK, lts);
		st = send(fd, &frame, sizeof(struct can_frame), 0);
		if(st < 0) {
			perror("send(fds)");
			goto close;
		}
		nanosleep(&ts, NULL);
	}
	
close:
	done = 1;
	return NULL;
}

void *recv_main(void *data) {
	struct cookie *cookie = (struct cookie*) data;
	int st, fd = cookie->fdr;
	struct timespec ts, *lts = cookie->lts;
	struct can_frame frame;
	
	long ns, cur_ns = 0, avg_ns = 0, max_ns = 0, min_ns = NS_SEC;
	long counter = 0;
	
#ifdef REALTIME
	struct sched_param param;
	param.sched_priority = 80;
	pthread_setschedparam(pthread_self(), SCHED_FIFO, &param);
#endif
	
	while(!done) {
		st = recv(fd, &frame, sizeof(struct can_frame), 0);
		if(st < 0) {
			perror("recv(fdr)");
			goto close;
		}
		
		clock_gettime(CLOCK, &ts);
		ns = NS_SEC*(ts.tv_sec - lts->tv_sec) + ts.tv_nsec - lts->tv_nsec;
		avg_ns = (avg_ns*counter + ns)/(counter + 1);
		cur_ns += ns;
		if(ns < min_ns)
			min_ns = ns;
		if(ns > max_ns)
			max_ns = ns;
		
		++counter;
		if(!(counter % FREQ)) {
			printf("%ld\t%ld\t%ld\t%ld\n", min_ns, max_ns, avg_ns, cur_ns/FREQ);
			cur_ns = 0;
		}
	}
	
close:
	done = 1;
	return NULL;
}

int main(int argc, char *argv[]) {
	int fds, fdr, ret = 0;
	struct timespec lts;
	struct cookie cookie;
	pthread_t send_thread, recv_thread;
	
	if(argc < 3) {
		printf("usage: %s <send-if> <recv-if>\n", argv[0]);
		goto quit;
	}
	
	//signal(SIGTERM, sighandler);
	//signal(SIGINT, sighandler);
	
	fds = can_socket(argv[1]);
	fdr = can_socket(argv[2]);
	if(fds < 0 || fdr < 0) {
		ret = 1;
		goto close;
	}
	
	printf("min\tmax\tavg\tcurrent\n");
	
	cookie.lts = &lts;
	cookie.fdr = fdr;
	cookie.fds = fds;
	
	pthread_create(&recv_thread, NULL, recv_main, &cookie);
	pthread_create(&send_thread, NULL, send_main, &cookie);
	
	pthread_join(send_thread, NULL);
	pthread_join(recv_thread, NULL);
	
close:
	if(fds >= 0)
		close(fds);
	if(fdr >= 0)
		close(fdr);
quit:
	printf("exit\n");
	return ret;
}
