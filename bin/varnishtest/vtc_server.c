/*-
 * Copyright (c) 2008-2010 Varnish Software AS
 * All rights reserved.
 *
 * Author: Poul-Henning Kamp <phk@phk.freebsd.dk>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include "config.h"

#include <sys/types.h>
#include <sys/socket.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vtc.h"

#include "vss.h"
#include "vtcp.h"

struct server {
	unsigned		magic;
#define SERVER_MAGIC		0x55286619
	char			*name;
	struct vtclog		*vl;
	VTAILQ_ENTRY(server)	list;
	char			run;

	unsigned		repeat;
	char			*spec;

	int			depth;
	int			sock;
	char			listen[256];
	struct vss_addr		**vss_addr;
	char			*addr;
	char			*port;
	char			aaddr[32];
	char			aport[32];

	pthread_t		tp;
};

static VTAILQ_HEAD(, server)	servers =
    VTAILQ_HEAD_INITIALIZER(servers);

/**********************************************************************
 * Server thread
 */

static void *
server_thread(void *priv)
{
	struct server *s;
	struct vtclog *vl;
	int i, j, fd;
	struct sockaddr_storage addr_s;
	struct sockaddr *addr;
	socklen_t l;

	CAST_OBJ_NOTNULL(s, priv, SERVER_MAGIC);
	assert(s->sock >= 0);

	vl = vtc_logopen(s->name);

	vtc_log(vl, 2, "Started on %s %s", s->aaddr, s->aport);
	for (i = 0; i < s->repeat; i++) {
		if (s->repeat > 1)
			vtc_log(vl, 3, "Iteration %d", i);
		addr = (void*)&addr_s;
		l = sizeof addr_s;
		fd = accept(s->sock, addr, &l);
		if (fd < 0)
			vtc_log(vl, 0, "Accepted failed: %s", strerror(errno));
		vtc_log(vl, 3, "accepted fd %d", fd);
		fd = http_process(vl, s->spec, fd, &s->sock);
		vtc_log(vl, 3, "shutting fd %d", fd);
		j = shutdown(fd, SHUT_WR);
		if (!VTCP_Check(j))
			vtc_log(vl, 0, "Shutdown failed: %s", strerror(errno));
		VTCP_close(&fd);
	}
	vtc_log(vl, 2, "Ending");
	return (NULL);
}

/**********************************************************************
 * Allocate and initialize a server
 */

static struct server *
server_new(const char *name)
{
	struct server *s;

	AN(name);
	ALLOC_OBJ(s, SERVER_MAGIC);
	AN(s);
	REPLACE(s->name, name);
	s->vl = vtc_logopen(name);
	AN(s->vl);
	if (*s->name != 's')
		vtc_log(s->vl, 0, "Server name must start with 's'");

	bprintf(s->listen, "127.0.0.1:%d", 0);
	AZ(VSS_parse(s->listen, &s->addr, &s->port));
	s->repeat = 1;
	s->depth = 10;
	s->sock = -1;
	VTAILQ_INSERT_TAIL(&servers, s, list);
	return (s);
}

/**********************************************************************
 * Clean up a server
 */

static void
server_delete(struct server *s)
{

	CHECK_OBJ_NOTNULL(s, SERVER_MAGIC);
	macro_def(s->vl, s->name, "addr", NULL);
	macro_def(s->vl, s->name, "port", NULL);
	macro_def(s->vl, s->name, "sock", NULL);
	vtc_logclose(s->vl);
	free(s->name);
	/* XXX: MEMLEAK (?) (VSS ??) */
	FREE_OBJ(s);
}

/**********************************************************************
 * Start the server thread
 */

static void
server_start(struct server *s)
{
	int naddr;

	CHECK_OBJ_NOTNULL(s, SERVER_MAGIC);
	vtc_log(s->vl, 2, "Starting server");
	if (s->sock < 0) {
		naddr = VSS_resolve(s->addr, s->port, &s->vss_addr);
		if (naddr != 1)
			vtc_log(s->vl, 0,
			    "Server s listen address not unique"
			    " \"%s\" resolves to (%d) sockets",
			    s->listen, naddr);
		s->sock = VSS_listen(s->vss_addr[0], s->depth);
		assert(s->sock >= 0);
		VTCP_myname(s->sock, s->aaddr, sizeof s->aaddr,
		    s->aport, sizeof s->aport);
		macro_def(s->vl, s->name, "addr", "%s", s->aaddr);
		macro_def(s->vl, s->name, "port", "%s", s->aport);
		macro_def(s->vl, s->name, "sock", "%s %s", s->aaddr, s->aport);
		/* Record the actual port, and reuse it on subsequent starts */
		if (!strcmp(s->port, "0"))
			REPLACE(s->port, s->aport);
	}
	vtc_log(s->vl, 1, "Listen on %s %s", s->addr, s->port);
	s->run = 1;
	AZ(pthread_create(&s->tp, NULL, server_thread, s));
}

/**********************************************************************
 * Wait for server thread to stop
 */

static void
server_wait(struct server *s)
{
	void *res;

	CHECK_OBJ_NOTNULL(s, SERVER_MAGIC);
	vtc_log(s->vl, 2, "Waiting for server");
	AZ(pthread_join(s->tp, &res));
	if (res != NULL && !vtc_stop)
		vtc_log(s->vl, 0, "Server returned \"%p\"",
		    (char *)res);
	s->tp = 0;
	VTCP_close(&s->sock);
	s->sock = -1;
	s->run = 0;
}

/**********************************************************************
 * Generate VCL backend decls for our servers
 */

void
cmd_server_genvcl(struct vsb *vsb)
{
	struct server *s;

	VTAILQ_FOREACH(s, &servers, list) {
		VSB_printf(vsb,
		    "backend %s { .host = \"%s\"; .port = \"%s\"; }\n",
		    s->name, s->aaddr, s->aport);
	}
}


/**********************************************************************
 * Server command dispatch
 */

void
cmd_server(CMD_ARGS)
{
	struct server *s, *s2;

	(void)priv;
	(void)cmd;
	(void)vl;

	if (av == NULL) {
		/* Reset and free */
		VTAILQ_FOREACH_SAFE(s, &servers, list, s2) {
			CHECK_OBJ_NOTNULL(s, SERVER_MAGIC);
			VTAILQ_REMOVE(&servers, s, list);
			if (s->run) {
				(void)pthread_cancel(s->tp);
				server_wait(s);
			}
			server_delete(s);
		}
		return;
	}

	assert(!strcmp(av[0], "server"));
	av++;

	VTAILQ_FOREACH(s, &servers, list)
		if (!strcmp(s->name, av[0]))
			break;
	if (s == NULL)
		s = server_new(av[0]);
	CHECK_OBJ_NOTNULL(s, SERVER_MAGIC);
	av++;

	for (; *av != NULL; av++) {
		if (vtc_error)
			break;
		if (!strcmp(*av, "-wait")) {
			if (!s->run)
				vtc_log(s->vl, 0, "Server not -started");
			server_wait(s);
			continue;
		}
		/*
		 * We do an implict -wait if people muck about with a
		 * running server.
		 */
		if (s->run)
			server_wait(s);

		assert(s->run == 0);
		if (!strcmp(*av, "-repeat")) {
			s->repeat = atoi(av[1]);
			av++;
			continue;
		}
		if (!strcmp(*av, "-listen")) {
			bprintf(s->listen, "%s", av[1]);
			AZ(VSS_parse(s->listen, &s->addr, &s->port));
			av++;
			continue;
		}
		if (!strcmp(*av, "-start")) {
			server_start(s);
			continue;
		}
		if (**av == '-')
			vtc_log(s->vl, 0, "Unknown server argument: %s", *av);
		s->spec = *av;
	}
}
