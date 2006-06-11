/*
 * Copyright (c) 2006 Bjorn Andersson <flex@kryo.se>, Erik Ekman <yarrick@kryo.se>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <err.h>
#include <pwd.h>
#include <arpa/inet.h>
#include <zlib.h>

#include "tun.h"
#include "dns.h"

#ifndef MAX
#define MAX(a,b) ((a)>(b)?(a):(b))
#endif

#define FRAMESIZE (64*1024)

int running = 1;

static void
sigint(int sig) {
	running = 0;
}

static int
tunnel(int tun_fd, int dns_fd)
{
	int i;
	int read;
	fd_set fds;
	struct timeval tv;
	struct tun_frame *frame;
	long buflen;
	char buf[64*1024];

	frame = malloc(FRAMESIZE);

	while (running) {
		tv.tv_sec = 1;
		tv.tv_usec = 0;

		FD_ZERO(&fds);
		if (!dns_sending()) 
			FD_SET(tun_fd, &fds);
		FD_SET(dns_fd, &fds);

		i = select(MAX(tun_fd, dns_fd) + 1, &fds, NULL, NULL, &tv);
		
		if(i < 0) {
			if (running) {
				warn("select");
			}
			return 1;
		}
		
		if(i == 0) {
			dns_ping(dns_fd);
		} else {
			if(FD_ISSET(tun_fd, &fds)) {
				read = read_tun(tun_fd, frame, FRAMESIZE);
				if (read > 0) {
					buflen = sizeof(buf);
					compress2(buf, &buflen, frame->data, read - 4, 9);
					dns_handle_tun(dns_fd, buf, buflen);
				}
			}
			if(FD_ISSET(dns_fd, &fds)) {
				read = dns_read(dns_fd, buf, FRAMESIZE-4);
				if (read > 0) {
					buflen = 64*1024-4;
					uncompress(frame->data, &buflen, buf, read);

					frame->flags = htons(0x0000);
#ifdef LINUX
					frame->proto = htons(0x0800);	// Linux wants ETH_P_IP
#else
					frame->proto = htons(0x0002);	// BSD wants AF_INET as long word
#endif
					
					write_tun(tun_fd, frame, buflen + 4);
					if (!dns_sending()) {
						dns_ping(dns_fd);
					}
				}
			} 
		}
	}

	free(frame);

	return 0;
}

extern char *__progname;

static void
usage() {
	printf("Usage: %s [-v] [-h] [-f] [-u user] nameserver topdomain\n", __progname);
	exit(2);
}

static void
help() {
	printf("iodine IP over DNS tunneling client\n");
	printf("Usage: %s [-v] [-h] [-f] [-u user] nameserver topdomain\n", __progname);
	printf("  -f is to keep running in foreground\n");
	printf("  -u name to drop privileges and run as user 'name'\n");
	exit(0);
}

static void
version() {
	char *svnver = "$Id$";
	printf("iodine IP over DNS tunneling client\n");
	printf("SVN version: %s\n", svnver);
	exit(0);
}

int
main(int argc, char **argv)
{
	int tun_fd;
	int dns_fd;
	int choice;
	char *username;
	struct passwd *pw;
	int foreground;

	username = NULL;
	foreground = 0;
	
	if (geteuid() != 0) {
		printf("Run as root and you'll be happy.\n");
		usage();
	}

	while ((choice = getopt(argc, argv, "vfhu:")) != -1) {
		switch(choice) {
		case 'v':
			version();
			break;
		case 'f':
			foreground = 1;
			break;
		case 'h':
			help();
			break;
		case 'u':
			username = optarg;
			break;
		default:
			usage();
			break;
		}
	}

	argc -= optind;
	argv += optind;
	
	if (argc != 2) {
		usage();
	}
	
	if(username) {
		pw = getpwnam(username);
		if (!pw) {
			printf("User %s does not exist!\n", username);
			usage();
		}
	}

	tun_fd = open_tun();
	dns_fd = open_dns(argv[0], argv[1]);

	signal(SIGINT, sigint);
	
	if (!foreground) {
		daemon(0, 0);
		umask(0);
		alarm(0);
	}
	
	if (username) {
		if (setgid(pw->pw_gid) < 0 || setuid(pw->pw_uid) < 0) {
			printf("Could not switch to user %s!\n", username);
			usage();
		}
		printf("Now running as user %s\n", username);
	}

	tunnel(tun_fd, dns_fd);

	printf("Closing tunnel\n");

	close_dns(dns_fd);
	close_tun(tun_fd);

	return 0;
}