// SPDX-License-Identifier: GPL-2.0-only
/* AP3216C 接近事件监视程序 */

#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define AP3216C_INPUT_NAME "ap3216c-proximity"
#define INPUT_EVENT_PREFIX "/dev/input/event"
#define MAX_EVENT_DEVICES 64
#define DEVICE_PATH_LEN 64
#define DEVICE_NAME_LEN 128
#define BITS_PER_LONG (sizeof(unsigned long) * 8)
#define SW_BITMAP_WORDS ((SW_MAX / BITS_PER_LONG) + 1)

static volatile sig_atomic_t running = 1;

static void handle_signal(int signo)
{
	(void)signo;
	running = 0;
}

static int install_signal_handlers(void)
{
	struct sigaction action;

	memset(&action, 0, sizeof(action));
	action.sa_handler = handle_signal;
	sigemptyset(&action.sa_mask);

	if (sigaction(SIGINT, &action, NULL) < 0 || sigaction(SIGTERM, &action, NULL) < 0) {
		perror("sigaction");
		return -1;
	}

	return 0;
}

static int read_device_name(int fd, char *name, size_t size)
{
	memset(name, 0, size);
	if (ioctl(fd, EVIOCGNAME(size), name) < 0)
		return -1;

	name[size - 1] = '\0';
	return 0;
}

/* event 编号由 input 子系统动态分配，按设备名称查找。 */
static int find_input_device(char *path, size_t size)
{
	char candidate[DEVICE_PATH_LEN];
	char name[DEVICE_NAME_LEN];
	int fd, i;

	for (i = 0; i < MAX_EVENT_DEVICES; i++) {
		snprintf(candidate, sizeof(candidate), "%s%d", INPUT_EVENT_PREFIX, i);
		fd = open(candidate, O_RDONLY | O_NONBLOCK);
		if (fd < 0)
			continue;

		if (read_device_name(fd, name, sizeof(name)) == 0 && strcmp(name, AP3216C_INPUT_NAME) == 0) {
			close(fd);
			if (snprintf(path, size, "%s", candidate) >= (int)size)
				return -1;
			return 0;
		}

		close(fd);
	}

	return -1;
}

/* EVIOCGSW 返回位图，可在首次中断前读取 input 核心保存的状态。 */
static int read_current_state(int fd, int *near)
{
	unsigned long switches[SW_BITMAP_WORDS] = { 0 };

	if (ioctl(fd, EVIOCGSW(sizeof(switches)), switches) < 0)
		return -1;

	*near = !!(switches[SW_FRONT_PROXIMITY / BITS_PER_LONG] & (1UL << (SW_FRONT_PROXIMITY % BITS_PER_LONG)));
	return 0;
}

static int monitor_events(int fd)
{
	struct pollfd poll_fd = {
		.fd = fd,
		.events = POLLIN,
	};
	struct input_event event;
	int state = -1;
	int near;
	ssize_t size;
	int ret;

	if (read_current_state(fd, &state) == 0)
		printf("Initial state: %s (%d)\n", state ? "NEAR" : "FAR", state);
	else
		perror("EVIOCGSW");

	printf("Monitoring proximity events. Press Ctrl+C to exit.\n");

	while (running) {
		/* 超时使循环定期检查退出信号。 */
		ret = poll(&poll_fd, 1, 500);
		if (ret < 0) {
			if (errno == EINTR)
				continue;
			perror("poll");
			return -1;
		}
		if (ret == 0)
			continue;

		if (poll_fd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
			fprintf(stderr, "Input device disconnected.\n");
			return -1;
		}
		if (!(poll_fd.revents & POLLIN))
			continue;

		size = read(fd, &event, sizeof(event));
		if (size < 0 && (errno == EINTR || errno == EAGAIN))
			continue;
		if (size != (ssize_t)sizeof(event)) {
			if (size < 0)
				perror("read");
			else
				fprintf(stderr, "Unexpected event size: %ld\n", (long)size);
			return -1;
		}

		if (event.type != EV_SW || event.code != SW_FRONT_PROXIMITY)
			continue;

		near = !!event.value;
		if (near == state)
			continue;

		state = near;
		printf("[%ld.%06ld] Proximity: %s (%d)\n", (long)event.time.tv_sec, (long)event.time.tv_usec, near ? "NEAR" : "FAR", near);
		fflush(stdout);
	}

	return 0;
}

int main(int argc, char *argv[])
{
	char path[DEVICE_PATH_LEN];
	char name[DEVICE_NAME_LEN];
	int fd, ret;

	if (argc == 1) {
		if (find_input_device(path, sizeof(path)) < 0) {
			fprintf(stderr, "Cannot find input device: %s\n", AP3216C_INPUT_NAME);
			return EXIT_FAILURE;
		}
	} else if (argc == 2) {
		if (snprintf(path, sizeof(path), "%s", argv[1]) >= (int)sizeof(path)) {
			fprintf(stderr, "Input device path is too long.\n");
			return EXIT_FAILURE;
		}
	} else {
		fprintf(stderr, "Usage: %s [/dev/input/eventX]\n", argv[0]);
		return EXIT_FAILURE;
	}

	if (install_signal_handlers() < 0)
		return EXIT_FAILURE;

	fd = open(path, O_RDONLY | O_NONBLOCK);
	if (fd < 0) {
		fprintf(stderr, "Cannot open %s: %s\n", path, strerror(errno));
		return EXIT_FAILURE;
	}

	if (read_device_name(fd, name, sizeof(name)) < 0)
		snprintf(name, sizeof(name), "unknown");

	printf("Device: %s (%s)\n", path, name);
	ret = monitor_events(fd);
	close(fd);

	return ret == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
