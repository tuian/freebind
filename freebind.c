#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>
#include <netinet/ip.h>
#include <stdlib.h>
#include <time.h>

#include "list.h"
#include "cidr.h"

int first_run = 1;
char *env_random;
buffer_t socket_cidrs_ipv4;
buffer_t socket_cidrs_ipv6;


void get_random_bytes(uint8_t *buf, size_t len) // not cryptographically secure
{
	for(size_t i = 0; i < len; i++)
	{
		buf[i] = rand();
	}
}

int get_random_address_from_cidr(cidr_t *cidr, buffer_t *buf)
{
	if(cidr->protocol == 4)
	{
		struct sockaddr_in *ip4addr = safe_malloc(sizeof(*ip4addr));
		ip4addr->sin_family = AF_INET;
		ip4addr->sin_port = 0;
		char random[4];
		get_random_bytes(random, sizeof(random));
		bitwise_clear(random, 0, cidr->mask);
		bitwise_xor((uint8_t*)&ip4addr->sin_addr.s_addr, random, cidr->prefix, sizeof(random));
		buf->len = sizeof(*ip4addr);
		buf->data = ip4addr;
	}
	else if(cidr->protocol == 6)
	{
		struct sockaddr_in6 *ip6addr = safe_malloc(sizeof(*ip6addr));
		bzero(ip6addr, sizeof(*ip6addr));
		ip6addr->sin6_family = AF_INET6;
		ip6addr->sin6_port = 0;
		char random[16];
		get_random_bytes(random, sizeof(random));
		bitwise_clear(random, 0, cidr->mask);
		bitwise_xor((uint8_t*)&ip6addr->sin6_addr.s6_addr, random, cidr->prefix, sizeof(random));
		buf->len = sizeof(*ip6addr);
		buf->data = ip6addr;
	}
	else
	{
		buf->len = 0;
		buf->data = NULL;
		return 0;
	}
	return 1;
}

void free_buf_array(buffer_t *arr)
{
	for(size_t i = 0; i < arr->len; i++)
	{
		free(((void**)arr->data)[i]);
	}
}

void cleanup()
{
	free_buf_array(&socket_cidrs_ipv4);
	free_buf_array(&socket_cidrs_ipv6);
}

void initialize()
{
	if(!first_run)
	{
		return;
	}
	first_run = 0;

	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	srand(ts.tv_sec + ts.tv_nsec);

	env_random = getenv("FREEBIND_RANDOM");
	if(env_random == NULL)
	{
		return;
	}

	single_list_t* cidr_list_ipv4 = single_list_new();
	single_list_t* cidr_list_ipv6 = single_list_new();
	char *token;
	char *remaining = env_random;
	while(token = strtok_r(remaining, ", ", &remaining))
	{
		cidr_t *cidr = safe_malloc(sizeof(*cidr));
		if(!cidr_from_string(cidr, token))
		{
			free(cidr);
			continue;
		}
		if(cidr->protocol == 4)
		{
			single_list_push_back(cidr_list_ipv4, cidr);
		}
		else if(cidr->protocol == 6)
		{
			single_list_push_back(cidr_list_ipv6, cidr);
		}
	}
	socket_cidrs_ipv4 = single_list_to_array(cidr_list_ipv4);
	single_list_free(cidr_list_ipv4);
	socket_cidrs_ipv6 = single_list_to_array(cidr_list_ipv6);
	single_list_free(cidr_list_ipv6);
	atexit(cleanup);
}

int socket(int domain, int type, int protocol)
{
	initialize();
	int (*original_socket)(int, int, int) = dlsym(RTLD_NEXT, "socket");
	int result = original_socket(domain, type, protocol);
	if(domain == PF_INET || domain == PF_INET6)
	{
		const int enable = 1;
		setsockopt(result, SOL_IP, IP_FREEBIND, &enable, sizeof(enable));

		buffer_t *socket_cidrs = &socket_cidrs_ipv4;
		if(domain == PF_INET6)
		{
			socket_cidrs = &socket_cidrs_ipv6;
		}
		if(socket_cidrs->len > 0)
		{
			buffer_t address;
			if(get_random_address_from_cidr(((cidr_t**)socket_cidrs->data)[rand() % socket_cidrs->len], &address))
			{
				bind(result, (struct sockaddr*)address.data, address.len);
				free(address.data);
			}
		}
	}
	return result;
}