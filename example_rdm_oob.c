#include <stdio.h>
#include <stdlib.h>
#include <getopt.h>
#include <netinet/in.h>
#include <inttypes.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <unistd.h>
#include <netinet/tcp.h>
#include <fcntl.h>
#include <rdma/fabric.h>
#include <rdma/fi_domain.h>
#include <rdma/fi_endpoint.h>
#include <rdma/fi_cm.h>

//Build with
//gcc -o example_rdm example_rdm.c -L<path to libfabric lib> -I<path to libfabric include> -lfabric

#define BUF_SIZE 64
#define MR_KEY 0xC0DE

char *src_addr = NULL, *dst_addr = NULL;
char *oob_port = "9228";
int listen_sock, oob_sock;
struct fi_info *hints, *info;
struct fid_fabric *fabric = NULL;
struct fid_domain *domain = NULL;
struct fid_ep *ep = NULL;
struct fid_av *av = NULL;
struct fid_cq *cq = NULL;
struct fid_mr *mr = NULL;
void *desc;
char buf[BUF_SIZE];
fi_addr_t fi_addr = FI_ADDR_UNSPEC;

static int sock_listen(char *node, char *service)
{
	struct addrinfo *ai, hints;
	int val, ret;

	memset(&hints, 0, sizeof hints);
	hints.ai_flags = AI_PASSIVE;

	ret = getaddrinfo(node, service, &hints, &ai);
	if (ret) {
		printf("getaddrinfo() %s\n", gai_strerror(ret));
		return ret;
	}

	listen_sock = socket(ai->ai_family, SOCK_STREAM, 0);
	if (listen_sock < 0) {
		printf("socket error");
		ret = listen_sock;
		goto out;
	}

	val = 1;
	ret = setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR,
			 (void *) &val, sizeof val);
	if (ret) {
		printf("setsockopt SO_REUSEADDR");
		goto out;
	}

	ret = bind(listen_sock, ai->ai_addr, ai->ai_addrlen);
	if (ret) {
		printf("bind");
		goto out;
	}

	ret = listen(listen_sock, 0);
	if (ret)
		printf("listen error");

out:
	if (ret && listen_sock >= 0)
		close(listen_sock);
	freeaddrinfo(ai);
	return ret;
}

static int sock_setup(int sock)
{
	int ret, op;
	long flags;

	op = 1;
	ret = setsockopt(sock, IPPROTO_TCP, TCP_NODELAY,
			  (void *) &op, sizeof(op));
	if (ret)
		return ret;

	flags = fcntl(sock, F_GETFL);
	if (flags < 0)
		return -errno;

	if (fcntl(sock, F_SETFL, flags))
		return -errno;

	return 0;
}

static int init_oob(void)
{
	struct addrinfo *ai = NULL;
	int ret;

	if (!dst_addr) {
		ret = sock_listen(src_addr, oob_port);
		if (ret)
			return ret;

		oob_sock = accept(listen_sock, NULL, 0);
		if (oob_sock < 0) {
			printf("accept error");
			ret = oob_sock;
			return ret;
		}

		close(listen_sock);
	} else {
		ret = getaddrinfo(dst_addr, oob_port, NULL, &ai);
		if (ret) {
			printf("getaddrinfo error");
			return ret;
		}

		oob_sock = socket(ai->ai_family, SOCK_STREAM, 0);
		if (oob_sock < 0) {
			printf("socket error");
			ret = oob_sock;
			goto free;
		}

		ret = connect(oob_sock, ai->ai_addr, ai->ai_addrlen);
		if (ret) {
			printf("connect error");
			close(oob_sock);
			goto free;
		}
		sleep(1);
	}

	ret = sock_setup(oob_sock);

free:
	if (ai)
		freeaddrinfo(ai);
	return ret;
}

static int sock_send(int fd, void *msg, size_t len)
{
	size_t sent;
	ssize_t ret, err = 0;

	for (sent = 0; sent < len; ) {
		ret = send(fd, ((char *) msg) + sent, len - sent, 0);
		if (ret > 0) {
			sent += ret;
		} else {
			err = -errno;
			break;
		}
	}

	return err ? err: 0;
}

static int sock_recv(int fd, void *msg, size_t len)
{
	size_t rcvd;
	ssize_t ret, err = 0;

	for (rcvd = 0; rcvd < len; ) {
		ret = recv(fd, ((char *) msg) + rcvd, len - rcvd, 0);
		if (ret > 0) {
			rcvd += ret;
		} else if (ret == 0) {
			err = -FI_ENOTCONN;
			break;
		} else {
			err = -errno;
			break;
		}
	}

	return err ? err: 0;
}

static int sync_progress(void)
{
	int ret, value = 0, result = -FI_EOTHER;

	if (dst_addr) {
		ret = send(oob_sock, &value, sizeof(value), 0);
		if (ret != sizeof(value))
			return -FI_EOTHER;

		do {
			ret = recv(oob_sock, &result, sizeof(result), MSG_DONTWAIT);
			if (ret == sizeof(result))
				break;

			ret = fi_cq_read(cq, NULL, 0);
			if (ret && ret != -FI_EAGAIN)
				return ret;
		} while (1);
	} else {
		do {
			ret = recv(oob_sock, &result, sizeof(result), MSG_DONTWAIT);
			if (ret == sizeof(result))
				break;

			ret = fi_cq_read(cq, NULL, 0);
			if (ret && ret != -FI_EAGAIN)
				return ret;
		} while (1);

		ret = send(oob_sock, &value, sizeof(value), 0);
		if (ret != sizeof(value))
			return -FI_EOTHER;
	}
	return 0;
}

static int exchange_addresses(void)
{
	char addr_buf[BUF_SIZE];
	int ret;
	size_t addrlen = BUF_SIZE;

	ret = fi_getname(&ep->fid, addr_buf, &addrlen);
	if (ret) {
		printf("fi_getname error %d\n", ret);
		return ret;
	}

	ret = sock_send(oob_sock, addr_buf, BUF_SIZE);
	if (ret) {
		printf("sock_send error %d\n", ret);
		return ret;
	}

	memset(addr_buf, 0, BUF_SIZE);
	ret = sock_recv(oob_sock, addr_buf, BUF_SIZE);
	if (ret) {
		printf("sock_recv error %d\n", ret);
		return ret;
	}

	ret = fi_av_insert(av, addr_buf, 1, &fi_addr, 0, NULL);
	if (ret != 1) {
		printf("av insert error\n");
		return -FI_ENOSYS;
	}

	return sync_progress();
}

static int initialize(void)
{
	struct fi_cq_attr cq_attr = {0};
	struct fi_av_attr av_attr = {0};
	char str_addr[BUF_SIZE];
	int ret;

	ret = init_oob();
        if (ret)
                return ret;

	ret = fi_getinfo(FI_VERSION(1,9), NULL, NULL, 0,
			 hints, &info);
	if (ret) {
		printf("fi_getinfo error (%d)\n", ret);
		return ret;
	}

	ret = fi_fabric(info->fabric_attr, &fabric, NULL);
	if (ret) {
		printf("fi_fabric error (%d)\n", ret);
		return ret;
	}

	ret = fi_domain(fabric, info, &domain, NULL);
	if (ret) {
		printf("fi_domain error (%d)\n", ret);
		return ret;
	}

	ret = fi_endpoint(domain, info, &ep, NULL);
	if (ret) {
		printf("fi_endpoint error (%d)\n", ret);
		return ret;
	}

	cq_attr.size = 128;
	cq_attr.format = FI_CQ_FORMAT_MSG;
	ret = fi_cq_open(domain, &cq_attr, &cq, NULL);
	if (ret) {
		printf("fi_cq_open error (%d)\n", ret);
		return ret;
	}

	ret = fi_ep_bind(ep, &cq->fid, FI_SEND | FI_RECV);
	if (ret) {
		printf("fi_ep_bind cq error (%d)\n", ret);
		return ret;
	}

	av_attr.type = FI_AV_TABLE;
	av_attr.count = 1;
	ret = fi_av_open(domain, &av_attr, &av, NULL);
	if (ret) {
		printf("fi_av_open error (%d)\n", ret);
		return ret;
	}

	ret = fi_ep_bind(ep, &av->fid, 0);
	if (ret) {
		printf("fi_ep_bind av error (%d)\n", ret);
		return ret;
	}

	ret = fi_enable(ep);
	if (ret) {
		printf("fi_enable error (%d)\n", ret);
		return ret;
	}

	ret = fi_mr_reg(domain, buf, BUF_SIZE, FI_SEND | FI_RECV | FI_WRITE | FI_READ |
			FI_REMOTE_WRITE | FI_REMOTE_READ, 0, MR_KEY, 0, &mr, NULL);
	if (ret) {
		printf("fi_mr_reg error (%d)\n", ret);
		return ret;
	}

	desc = fi_mr_desc(mr);

	if (info->domain_attr->mr_mode & FI_MR_ENDPOINT) {
		ret = fi_mr_bind(mr, &ep->fid, 0);
		if (ret) {
			printf("fi_mr_bind error (%d)\n", ret);
			return ret;
		}

		/*
		ret = fi_mr_bind(mr, &rxcntr->fid, FI_REMOTE_WRITE);
		if (ret)
			return ret;
		*/

		ret = fi_mr_enable(mr);
		if (ret) {
			printf("fi_mr_enable error (%d)\n", ret);
			return ret;
		}
	}

	ret = exchange_addresses();
	if (ret)
		return ret;

	return 0;
}

static void cleanup(void)
{
	int ret;

	if (mr) {
		ret = fi_close(&mr->fid);
		if (ret)
			printf("warning: error closing EP (%d)\n", ret);
	}

	if (ep) {
		ret = fi_close(&ep->fid);
		if (ret)
			printf("warning: error closing EP (%d)\n", ret);
	}

	if (av) {
		ret = fi_close(&av->fid);
		if (ret)
			printf("warning: error closing AV (%d)\n", ret);
	}

	if (cq) {
		ret = fi_close(&cq->fid);
		if (ret)
			printf("warning: error closing CQ (%d)\n", ret);
	}

	if (domain) {
		ret = fi_close(&domain->fid);
		if (ret)
			printf("warning: error closing domain (%d)\n", ret);
	}

	if (fabric) {
		ret = fi_close(&fabric->fid);
		if (ret)
			printf("warning: error closing fabric (%d)\n", ret);
	}

	if (info)
		fi_freeinfo(info);
}

static int post_recv(void)
{
	int ret;

	do {
		ret = fi_recv(ep, buf, BUF_SIZE, NULL, fi_addr, NULL);
		if (ret && ret != -FI_EAGAIN) {
			printf("error posting recv buffer (%d\n", ret);
			return ret;
		}
		if (ret == -FI_EAGAIN)
			(void) fi_cq_read(cq, NULL, 0);
	} while (ret);

	return 0;
}

static int post_send(void)
{
	char *msg = "Hello, server! I am the client you've been waiting for!\0";
	int ret;

	(void) snprintf(buf, BUF_SIZE, "%s", msg);

	do {
		ret = fi_send(ep, buf, strlen(msg), desc, fi_addr, NULL);
		if (ret && ret != -FI_EAGAIN) {
			printf("error posting send buffer (%d)\n", ret);
			return ret;
		}
		if (ret == -FI_EAGAIN)
			(void) fi_cq_read(cq, NULL, 0);
	} while (ret);

	return 0;
}

static int wait_cq(void)
{
	struct fi_cq_err_entry comp;
	int ret;

	do {
		ret = fi_cq_read(cq, &comp, 1);
		if (ret < 0 && ret != -FI_EAGAIN) {
			printf("error reading cq (%d)\n", ret);
			return ret;
		}
	} while (ret != 1);

	if (comp.flags & FI_RECV)
		printf("I received a message!\n");
	else if (comp.flags & FI_SEND)
		printf("My sent message got sent!\n");

	return 0;
}

static int run(void)
{
	int ret;

	if (dst_addr) {
		printf("Client: send to server %s\n", dst_addr);

		ret = post_send();
		if (ret)
			return ret;

		ret = wait_cq();
		if (ret)
			return ret;

	} else {
		printf("Server: post buffer and wait for message from client\n");

		ret = post_recv();
		if (ret)
			return ret;

		ret = wait_cq();
		if (ret)
			return ret;

		printf("This is the message I received: %s\n", buf);
	}

	return sync_progress();
}

int main(int argc, char **argv)
{
	int op, ret;

	hints = fi_allocinfo();
	if (!hints)
		return EXIT_FAILURE;

	while ((op = getopt(argc, argv, "s:")) != -1) {
		switch (op) {
		case 's':
			src_addr = optarg;
			break;
		default:
			printf("argument unknown\n");
		}
	}

	dst_addr = argv[optind];

	hints->ep_attr->type = FI_EP_RDM;
	hints->caps = FI_MSG;
	hints->tx_attr->op_flags = FI_DELIVERY_COMPLETE;
	hints->domain_attr->mr_mode = FI_MR_ENDPOINT | FI_MR_LOCAL |
		FI_MR_PROV_KEY | FI_MR_ALLOCATED | FI_MR_VIRT_ADDR;
	hints->fabric_attr->prov_name = "tcp";

	ret = init_oob();
	if (ret)
		return ret;

	ret = initialize();
	if (ret)
		goto out;

	ret = run();
out:
	cleanup();
	return ret;
}
