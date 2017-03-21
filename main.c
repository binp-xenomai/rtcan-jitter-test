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

#ifdef __XENO__
#define REALTIME
#endif // __XENO__

#define CLOCK CLOCK_MONOTONIC

#define ASYNC

//#define STATS
#ifdef STATS
#define STATS_SIZE 0x100000 // 2^20
#endif // STATS

//#define DBGPRINT

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

void frame_init(struct can_frame *frame) {
	frame->can_id = 0xf;
	frame->can_dlc = 1;
	frame->data[0] = 0xff;
}

int done = 0;

void sighandler(int signo) {
	done = 1;
}

struct cookie {
	int fds, fdr;
	struct timespec *lts;
	int allow;
#ifdef STATS
	long *stats;
#endif
};

#define NS_SEC 1000000000l
#define FREQ   1000

int stack_counter = 0;

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

	frame_init(&frame);
	
	ts.tv_sec = 0;
	ts.tv_nsec = NS_SEC/FREQ;
	
	while(!done) {
		nanosleep(&ts, NULL);
		
		if(cookie->allow)
			cookie->allow = 0;
		else
			continue;
		
		clock_gettime(CLOCK, lts);
		st = send(fd, &frame, sizeof(struct can_frame), 0);
		if(st < 0) {
			perror("send(fds)");
			goto close;
		} 
#ifdef DBGPRINT
		else {
			printf("send(fds) -> %d\n", st);
		}
#endif // DBGPRINT
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
#ifdef DBGPRINT
		else {
			printf("recv(fdr) -> %d\n", st);
		}
#endif // DBGPRINT
		
		clock_gettime(CLOCK, &ts);
		
		cookie->allow = 1;
		
		ns = NS_SEC*(ts.tv_sec - lts->tv_sec) + ts.tv_nsec - lts->tv_nsec;
		cur_ns += ns;
		if(ns < min_ns)
			min_ns = ns;
		if(ns > max_ns)
			max_ns = ns;
		
#ifdef STATS
		cookie->stats[counter] = ns;
		if(counter + 1 >= STATS_SIZE) {
			done = 1;
		} 
#endif // STATS
		
		++counter;
		if(!(counter % FREQ)) {
			printf("%ld\t%ld\t%ld\n", min_ns, max_ns, cur_ns/FREQ);
			cur_ns = 0;
			min_ns = NS_SEC;
			max_ns = 0;
		}
	}
	
close:
	done = 1;
	return NULL;
}

int main(int argc, char *argv[]) {
	int fds, fdr, ret = 0;
	
#ifdef STATS
	FILE *file;
	long *stats;
	int i;
#endif // STATS
	
	if(argc < 3) {
		printf("usage: %s <send-if> <recv-if>\n", argv[0]);
		goto quit;
	}

#ifdef STATS
	/* alloc memory to store stats */
	stats = (long *) malloc(sizeof(long)*STATS_SIZE);
#endif // STATS
	
	//signal(SIGTERM, sighandler);
	//signal(SIGINT, sighandler);
	
	fds = can_socket(argv[1]);
	fdr = can_socket(argv[2]);
	if(fds < 0 || fdr < 0) {
		ret = 1;
		goto close;
	}
	
	printf("min\tmax\tcurrent\n");

#ifdef ASYNC
	struct timespec lts;
	struct cookie cookie;
	pthread_t send_thread, recv_thread;

	cookie.lts = &lts;
	cookie.fdr = fdr;
	cookie.fds = fds;
	cookie.allow = 1;
#ifdef STATS
	cookie.stats = stats;
#endif // STATS

	pthread_create(&recv_thread, NULL, recv_main, &cookie);
	pthread_create(&send_thread, NULL, send_main, &cookie);
	
	pthread_join(send_thread, NULL);
	pthread_join(recv_thread, NULL);
#else // ASYNC

	int st, allow = 1;
	struct timespec ts, lts, sleepts;
	struct can_frame frame;

#ifdef REALTIME
	struct sched_param param;
	param.sched_priority = 80;
	pthread_setschedparam(pthread_self(), SCHED_FIFO, &param);
#endif

	sleepts.tv_sec = 0;
	sleepts.tv_nsec = NS_SEC/FREQ;

	struct timeval tv;
	fd_set set;

	long ns, cur_ns = 0, avg_ns = 0, max_ns = 0, min_ns = NS_SEC;
	long counter = 0;

	while(!done) {

		if(allow) {
			allow = 0;

			nanosleep(&sleepts, NULL);

			frame_init(&frame);

			clock_gettime(CLOCK, &lts);
			st = send(fds, &frame, sizeof(struct can_frame), 0);
			if(st < 0) {
				perror("send(fds)");
				goto close;
			} else {
				// printf("send(fds) returns %d\n", st);
			}
		}

		FD_ZERO(&set);
		FD_SET(fdr, &set);
		tv.tv_sec = 1;
		tv.tv_usec = 0;

		// printf("fdr select\n");
		st = select(fdr + 1, &set, NULL, NULL, &tv);
		if(st < 0) {
			perror("select(fdr)");
			goto close;
		} else if(st == 0) {
			// printf("fdr timeout\n");
		} else if(st > 0) {
			// printf("fdr recv\n");

			st = recv(fdr, &frame, sizeof(struct can_frame), 0);
			if(st < 0) {
				perror("recv(fdr)");
				goto close;
			}

			clock_gettime(CLOCK, &ts);

			ns = NS_SEC*(ts.tv_sec - lts.tv_sec) + ts.tv_nsec - lts.tv_nsec;
			avg_ns = (avg_ns*counter + ns)/(counter + 1);
			cur_ns += ns;
			if(ns < min_ns)
				min_ns = ns;
			if(ns > max_ns)
				max_ns = ns;

	#ifdef STATS
			stats[counter] = ns;
			if(counter + 1 >= STATS_SIZE) {
				done = 1;
			}
	#endif // STATS

			++counter;
			if(!(counter % FREQ)) {
				printf("%ld\t%ld\t%ld\t%ld\n", min_ns, max_ns, avg_ns, cur_ns/FREQ);
				cur_ns = 0;
			}

			allow = 1;
		}
	}

#endif // ASYNC
	
close:
	if(fds >= 0)
		close(fds);
	if(fdr >= 0)
		close(fdr);
		
#ifdef STATS
print_stats:
	/* dump stats and free mem */
	file = fopen("stats.txt", "w");
	for(i = 0; i < STATS_SIZE; ++i) {
		fprintf(file, "%ld\n", stats[i]);
	}
	fclose(file);
	free(stats);
#endif // STATS

quit:
	printf("exit\n");
	return ret;
}
