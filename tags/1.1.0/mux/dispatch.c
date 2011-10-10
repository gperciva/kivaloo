#include <assert.h>
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>

#include "mpool.h"
#include "netbuf.h"
#include "network.h"
#include "wire.h"
#include "warnp.h"

#include "dispatch.h"

/* Dispatcher state. */
struct dispatch_state {
	/* Listening sockets. */
	struct sock_listen * sock_listen;	/* Listening sockets. */
	size_t nsock_listen;			/* # listening sockets. */

	/* Active sockets. */
	struct sock_active * sock_active;	/* Active sockets. */
	size_t nsock_active;			/* # active sockets. */
	size_t nsock_active_max;		/* Max # active sockets. */

	/* Request queue. */
	struct wire_requestqueue * Q;		/* Connected to target. */
	int failed;				/* Q has failed. */
};

/* Listening socket. */
struct sock_listen {
	struct dispatch_state * dstate;		/* Dispatcher. */
	int s;					/* Listening socket. */
	void * accept_cookie;			/* From network_accept. */
};

/* Connected client. */
struct sock_active {
	/* Bookkeeping. */
	struct dispatch_state * dstate;		/* Dispatcher. */
	struct sock_active * next;		/* Next in linked list. */
	struct sock_active * prev;		/* Previous in linked list. */

	/* The connection. */
	int s;					/* Connected socket. */
	struct netbuf_read * readq;		/* Packet read queue. */
	struct netbuf_write * writeq;		/* Packet write queue. */
	void * read_cookie;			/* Packet read cookie. */
	size_t nrequests;			/* # responses we owe. */
};

/* In-flight request state. */
struct forwardee {
	struct sock_active * conn;		/* Request origin. */
	struct wire_packet * P;			/* The request. */
};

MPOOL(forwardee, struct forwardee, 32768);

static void accept_stop(struct dispatch_state *);
static int accept_start(struct dispatch_state *);
static int callback_gotconn(void *, int);
static int readreq(struct sock_active *);
static int callback_gotrequest(void *, struct wire_packet *);
static int callback_gotresponse(void *, uint8_t *, size_t);
static int callback_writresponse(void *, int);
static int reqdone(struct sock_active *);
static int dropconn(struct sock_active *);

static void
accept_stop(struct dispatch_state * dstate)
{
	struct sock_listen * L;
	size_t i;

	/* Iterate through the sockets, cancelling any accepts. */
	for (i = 0; i < dstate->nsock_listen; i++) {
		L = &dstate->sock_listen[i];
		if (L->accept_cookie != NULL) {
			network_accept_cancel(L->accept_cookie);
			L->accept_cookie = NULL;
		}
	}
}

static int
accept_start(struct dispatch_state * dstate)
{
	struct sock_listen * L;
	size_t i;

	/* Make sure we don't have any in-progress accepts. */
	for (i = 0; i < dstate->nsock_listen; i++) {
		L = &dstate->sock_listen[i];
		if (L->accept_cookie != NULL) {
			warn0("Already trying to accept a connection!");
			goto err0;
		}
	}

	/* Try to accept connections. */
	for (i = 0; i < dstate->nsock_listen; i++) {
		L = &dstate->sock_listen[i];
		if ((L->accept_cookie =
		    network_accept(L->s, callback_gotconn, L)) == NULL)
			goto err1;
	}

	/* Success! */
	return (0);

err1:
	/* Cancel the accepts we started. */
	accept_stop(dstate);
err0:
	/* Failure! */
	return (-1);
}

static int
callback_gotconn(void * cookie, int s)
{
	struct sock_listen * L = cookie;
	struct dispatch_state * dstate = L->dstate;
	struct sock_active * S;

	/* This listener is no longer accepting. */
	L->accept_cookie = NULL;

	/* Check if the accept failed. */
	if (s == -1) {
		warnp("Error accepting connection");
		goto err0;
	}

	/* Stop trying to accept connections. */
	accept_stop(dstate);

	/* Allocate an active connection structure. */
	if ((S = malloc(sizeof(struct sock_active))) == NULL)
		goto err1;
	S->dstate = dstate;
	S->next = NULL;
	S->prev = NULL;
	S->s = s;
	S->read_cookie = NULL;
	S->nrequests = 0;

	/* Make the accepted connection non-blocking. */
	if (fcntl(S->s, F_SETFL, O_NONBLOCK) == -1) {
		warnp("Cannot make connection non-blocking");
		goto err2;
	}

	/* Create a buffered writer for the connection. */
	if ((S->writeq = netbuf_write_init(S->s)) == NULL) {
		warnp("Cannot create packet write queue");
		goto err2;
	}

	/* Create a buffered reader for the connection. */
	if ((S->readq = netbuf_read_init(S->s)) == NULL) {
		warn0("Cannot create packet read queue");
		goto err3;
	}

	/* Start listening for packets. */
	if (readreq(S))
		goto err4;

	/* Add this connection to the list. */
	S->next = dstate->sock_active;
	if (S->next != NULL)
		S->next->prev = S;
	dstate->sock_active = S;

	/* We have a connection.  Do we want more? */
	if (++dstate->nsock_active < dstate->nsock_active_max) {
		if (accept_start(dstate))
			goto err0;
	}

	/* Success! */
	return (0);

err4:
	netbuf_read_free(S->readq);
err3:
	netbuf_write_destroy(S->writeq);
	netbuf_write_free(S->writeq);
err2:
	free(S);
err1:
	close(s);
err0:
	/* Failure! */
	return (-1);
}

static int
readreq(struct sock_active * S)
{

	/* We shoudln't be reading yet. */
	assert(S->read_cookie == NULL);

	/* Read a request. */
	if ((S->read_cookie =
	    wire_readpacket(S->readq, callback_gotrequest, S)) == NULL) {
		warnp("Error reading request from connection");
		goto err0;
	}

	/* Success! */
	return (0);

err0:
	/* Failure! */
	return (-1);
}

static int
readreq_cancel(struct sock_active * S)
{

	/* Stop reading a request. */
	wire_readpacket_cancel(S->read_cookie);
	S->read_cookie = NULL;

	/* Drop the connection if it is now dead. */
	if ((S->nrequests == 0) && dropconn(S))
		goto err0;

	/* Success! */
	return (0);

err0:
	/* Failure! */
	return (-1);
}

static int
callback_gotrequest(void * cookie, struct wire_packet * P)
{
	struct sock_active * S = cookie;
	struct dispatch_state * dstate = S->dstate;
	struct forwardee * F;

	/* We're not reading a request any more. */
	S->read_cookie = NULL;

	/* Did we fail to read? */
	if (P == NULL) {
		/* If we have no request in progress, kill the connection. */
		if (S->nrequests == 0) {
			if (dropconn(S))
				goto err0;
		}

		/* Nothing to do. */
		goto done;
	}

	/* Bake a cookie. */
	if ((F = mpool_forwardee_malloc()) == NULL)
		goto err1;
	F->conn = S;
	F->P = P;
	S->nrequests++;

	/* Send the request to the target. */
	if (wire_requestqueue_add(dstate->Q, P->buf, P->len,
	    callback_gotresponse, F))
		goto err2;

	/* Read another request. */
	if (readreq(S))
		goto err0;

done:
	/* Success! */
	return (0);

err2:
	mpool_forwardee_free(F);
err1:
	free(P->buf);
	wire_packet_free(P);
err0:
	/* Failure! */
	return (-1);
}

static int
callback_gotresponse(void * cookie, uint8_t * buf, size_t buflen)
{
	struct forwardee * F = cookie;
	struct sock_active * S = F->conn;
	struct sock_active * S_next;

	/* Free the request packet buffer. */
	free(F->P->buf);

	/* Did this request fail? */
	if (buf == NULL)
		goto failed;

	/* Turn the request packet into a response packet. */
	F->P->buf = buf;
	F->P->len = buflen;

	/* Send the response packet back to the client. */
	if (wire_writepacket(S->writeq, F->P, callback_writresponse, F))
		goto err1;

	/* Success! */
	return (0);

failed:
	/* Free the packet and our cookie. */
	wire_packet_free(F->P);
	mpool_forwardee_free(F);

	/* We've finished with a request. */
	if (reqdone(S))
		goto err0;

	/* Stop trying to accept connections. */
	accept_stop(S->dstate);

	/* The connection to the upstream server has failed. */
	S->dstate->failed = 1;

	/* Stop reading requests from connections. */
	while (S != NULL) {
		S_next = S->next;
		if ((S->read_cookie != NULL) && readreq_cancel(S))
			goto err0;
		S = S_next;
	}

	/* The failed request has been successfully handled. */
	return (0);

err1:
	free(buf);
	wire_packet_free(F->P);
	mpool_forwardee_free(F);
err0:
	/* Failure! */
	return (-1);
}

static int
callback_writresponse(void * cookie, int failed)
{
	struct forwardee * F = cookie;
	struct sock_active * S = F->conn;

	(void)failed; /* UNUSED */

	/* Free the response packet (including buffer). */
	free(F->P->buf);
	wire_packet_free(F->P);

	/* Free the cookie. */
	mpool_forwardee_free(F);

	/* We've finished with a request. */
	if (reqdone(S))
		goto err0;

	/* Success! */
	return (0);

err0:
	/* Failure! */
	return (-1);
}

static int
reqdone(struct sock_active * S)
{

	/* We've finished a request. */
	S->nrequests--;

	/* Is this connection dead? */
	if ((S->nrequests == 0) && (S->read_cookie == NULL)) {
		if (dropconn(S))
			goto err0;
	}

	/* Success! */
	return (0);

err0:
	/* Failure! */
	return (-1);
}

static int
dropconn(struct sock_active * S)
{
	struct dispatch_state * dstate = S->dstate;

	/* Sanity check. */
	assert(S->read_cookie == NULL);
	assert(S->nrequests == 0);

	/* Detach from the dispatcher. */
	if (S->prev == NULL)
		dstate->sock_active = S->next;
	else
		S->prev->next = S->next;
	if (S->next != NULL)
		S->next->prev = S->prev;
	if (dstate->nsock_active-- == dstate->nsock_active_max) {
		if (accept_start(dstate))
			goto err3;
	}

	/* Free the buffered reader. */
	netbuf_read_free(S->readq);

	/*
	 * Shut down and free the buffered writer.  We don't need to wait for
	 * callbacks from netbuf_write to be performed, because they can only
	 * be pending if nrequests > 0.
	 */
	if (netbuf_write_destroy(S->writeq))
		goto err2;
	netbuf_write_free(S->writeq);

	/* Close the socket. */
	while (close(S->s)) {
		if (errno == EINTR)
			continue;
		warnp("close");
		goto err1;
	}

	/* Free the connection state. */
	free(S);

	/* Success! */
	return (0);

err3:
	netbuf_read_free(S->readq);
	netbuf_write_destroy(S->writeq);
err2:
	netbuf_write_free(S->writeq);
	close(S->s);
err1:
	free(S);

	/* Failure! */
	return (-1);
}

/**
 * dispatch_init(socks, nsocks, Q, maxconn):
 * Initialize a dispatcher to accept connections from the listening sockets
 * ${socks}[0 .. ${nsocks} - 1] (but no more than ${maxconn} at once) and
 * shuttle requests/responses to/from the request queue ${Q}.
 */
struct dispatch_state *
dispatch_init(const int * socks, size_t nsocks,
    struct wire_requestqueue * Q, size_t maxconn)
{
	struct dispatch_state * dstate;
	size_t i;

	/* Bake a cookie. */
	if ((dstate = malloc(sizeof(struct dispatch_state))) == NULL)
		goto err0;
	dstate->nsock_listen = nsocks;
	dstate->sock_active = NULL;
	dstate->nsock_active = 0;
	dstate->nsock_active_max = maxconn;
	dstate->Q = Q;
	dstate->failed = 0;

	/* Allocate an array of listeners. */
	if ((dstate->sock_listen =
	    malloc(nsocks * sizeof(struct sock_listen))) == NULL)
		goto err1;
	for (i = 0; i < nsocks; i++) {
		dstate->sock_listen[i].dstate = dstate;
		dstate->sock_listen[i].s = socks[i];
		dstate->sock_listen[i].accept_cookie = NULL;
	}

	/* Start accepting connections. */
	if (accept_start(dstate))
		goto err2;

	/* Success! */
	return (dstate);

err2:
	free(dstate->sock_listen);
err1:
	free(dstate);
err0:
	/* Failure! */
	return (NULL);
}

/**
 * dispatch_alive(dstate):
 * Return non-zero if the dispatcher with state ${dstate} is still alive.
 */
int
dispatch_alive(struct dispatch_state * dstate)
{

	/*
	 * The dispatcher is alive if its connection to the target has not
	 * failed, or if it has any connections to clients (i.e., if they
	 * haven't been cleaned up yet).
	 */
	return ((dstate->failed == 0) || (dstate->nsock_active > 0));
}

/**
 * dispatch_done(dstate):
 * Clean up the dispatcher state ${dstate}.
 */
void
dispatch_done(struct dispatch_state * dstate)
{

	/* Sanity-check. */
	assert(dstate->failed == 1);
	assert(dstate->sock_active == NULL);
	assert(dstate->nsock_active == 0);

	/* Free memory. */
	free(dstate->sock_listen);
	free(dstate);
}