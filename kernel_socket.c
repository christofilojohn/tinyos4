
#include "tinyos.h"
#include "kernel_streams.h"
#include "kernel_socket.h"
#include "kernel_cc.h"

/**
 * The port table (all ports available on the system).
 * Sockets are bound to a port on this table when created, 
 * and the socket->port is basically the index of socket in
 * the port table. 
*/
socket_cb* PORT_MAP[MAX_PORT];

socket_cb* init_socket_cb(FCB* parent_fcb){
    socket_cb *socket = (socket_cb*) xmalloc(sizeof(socket_cb));

    socket->fcb = parent_fcb;
    socket->refcount = 0;
    socket->type = SOCKET_UNBOUND;
    socket->port = NOPORT;
	rlnode_init(& socket->unbound_s.socket_node, &socket->unbound_s.socket_node);

    return socket;
}

static file_ops socket_file_ops = {
	.Read = socket_read,
	.Write = socket_write,
	.Close = socket_close
};

Fid_t sys_Socket(port_t port)
{
	Fid_t fid;
	FCB* fcb;
    
	// check if the given port number is valid
	if(port < 0 || port > MAX_PORT){
		return -1;
	}

	// Fail if FCB reservation fails
	if(!FCB_reserve(1, &fid, &fcb)){
		return -1;		
	}

	// get the socket
	socket_cb *socket = init_socket_cb(fcb);

	// connect the fcb to the pipe and to the ops
	fcb->streamobj = socket;
	fcb->streamfunc = &socket_file_ops;
	socket->port = port;

	return fid;
}

int sys_Listen(Fid_t sock)
{
	// get the FCB from sock and do the checks needed
	// 	port occupied by another listener
	// 	socket already initialized

	// check if given fid is legal and corresponding FCB is NULL
	if(sock < 0 || sock > MAX_FILEID-1 || get_fcb(sock)==NULL){
		//fprintf(stderr, "Sock invalid or getfcb null\n");
		return -1;
	}

	// get the socket that will be a listener
	socket_cb* listener_sock = (socket_cb*) get_fcb(sock)->streamobj;

	// check if the socket is NULL or its port is NOPORT
	if(listener_sock==NULL || listener_sock->port==NOPORT){
		//fprintf(stderr, "%s\n", listener_sock==NULL ? "Listener sock null" : "listener port noport\n");
		return -1;
	}

	// check if port is occupied by another listener
	if(PORT_MAP[listener_sock->port] != NULL){
		//fprintf(stderr, "port map [sock] occupied\n");
		return -1;
	}

	// check if socket is already initialized
	if(listener_sock->type != SOCKET_UNBOUND){
		//fprintf(stderr, "Socket type not unbound\n");
		return -1;
	}

	// install the socket on the port map
	PORT_MAP[listener_sock->port] = listener_sock;

	// mark the socket as listener
	listener_sock->type = SOCKET_LISTENER;
	
	// initialize the listener_socket fields of the union
	rlnode_init(& listener_sock->listener_s.queue, NULL);	// initialize the request queue of the listener to NULL (it's still empty)
	listener_sock->listener_s.req_available = COND_INIT;	// initialize the conditional variable on which the listener will sleep


	return 0;
}

Fid_t sys_Accept(Fid_t lsock)
{
	/*
	Possible reasons for error: 
	- the available file ids for the process are exhausted 
	- while waiting, the listening socket lsock was closed
	*/

	// check if given fid is legal and corresponding FCB is NULL
	if(lsock < 0 || lsock > MAX_FILEID-1 || get_fcb(lsock)==NULL){
		//fprintf(stderr, "Accept failed because of illegal sock\n");
		return -1;
	}	
	
	// get the socket that will start listening
	socket_cb* listener = (socket_cb*) get_fcb(lsock)->streamobj;

	// check if the file id is not initialized by Listen() 
	if(listener==NULL || listener->type!=SOCKET_LISTENER){
		//fprintf(stderr, "Accept failed because of null or problematic listener\n");
		return -1;
	}

	// the check for available FID in the current process will be done after waking up	

	// increase listener socket's refcount
	listener->refcount++;

	// wait for a request
	//fprintf(stderr, "Accept sleeping on 0x%X\n", & listener->listener_s.req_available);
	while(is_rlist_empty(&listener->listener_s.queue)){
		kernel_wait(& listener->listener_s.req_available, SCHED_USER);	// waiting cause is sched user because the user requested it.
	}
	//fprintf(stderr, "Accept woke up from 0x%X\n", & listener->listener_s.req_available);

	// wake up when a request is found

	// check if the port is still valid and correctly installed in the port map (the socket may have been closed while we were sleeping)
	if(listener==NULL || listener->type!=SOCKET_LISTENER || PORT_MAP[listener->port]!=listener){
		//fprintf(stderr, "Accept failed because of null or problematic listener after waking up\n");
		return -1;
	}

	// take the first request from the queue and try to honor it
	connection_request* request = (connection_request*) rlist_pop_front(& listener->listener_s.queue)->obj;
	request->admitted = 1;

	// try to construct peer (bound on the same port as the listener)
	Fid_t peer_fid = sys_Socket(listener->port);

	if(peer_fid==-1 || get_fcb(peer_fid)==NULL){
		// TODO should we put the request back in the waiting queue here?
		//fprintf(stderr, "ABORT ABORT\n");
		return -1;
	}

	// get the two peers
	socket_cb *peer_socket = (socket_cb*) get_fcb(peer_fid)->streamobj;
	socket_cb *req__socket = request->peer;

	// set the fields of each of the peers
	peer_socket->type = SOCKET_PEER;
	req__socket->type = SOCKET_PEER;

	// create and initialize the pipes
	pipe_cb *pipe1 = init_pipe_obj();
	pipe_cb *pipe2 = init_pipe_obj();

	// set the pipes' reader and writer fields
	pipe1->reader = peer_socket->fcb;
	pipe1->writer = req__socket->fcb;
	pipe2->reader = req__socket->fcb;
	pipe2->writer = peer_socket->fcb;

	// set the peer's pipe fields
	peer_socket->peer_s.read_pipe = pipe1;
	peer_socket->peer_s.write_pipe = pipe2;
	req__socket->peer_s.read_pipe = pipe2;
	req__socket->peer_s.write_pipe = pipe1;

	// signal the Connect side
	//fprintf(stderr, "accept signaling 0x%X and request->admitted=%d\n", & request->connected_cv, request->admitted);
	kernel_signal(& request->connected_cv);

	// in the end decrease refcount (TODO check if necessary)
	listener->refcount--;

	//fprintf(stderr, "Accept exiting successfully and returning %d\n", peer_fid);
	return peer_fid;
}

int sys_Connect(Fid_t sock, port_t port, timeout_t timeout)
{
	/*
	Possible reasons for error: 
		- the file id sock is not legal (i.e., an unconnected, non-listening socket) 
		- the given port is illegal. 
		- the port does not have a listening socket bound to it by Listen. 
		- the timeout has expired without a successful connection.
	*/

	// check if given fid is legal and corresponding FCB is NULL
	if(sock < 0 || sock > MAX_FILEID-1 || get_fcb(sock)==NULL){
		fprintf(stderr, "illegal sock or fcb null\n");	
		return -1;
	}
	socket_cb *cursoc = (socket_cb*) get_fcb(sock)->streamobj;	// the socket that makes the request

	// check if the given port is illegal
	if(port <= NOPORT || port > MAX_PORT-1){
		fprintf(stderr, "illegal port %d\n", port);	
		return -1;
	}

	// check if the port does not havea (listening) socket bound to it
	if(PORT_MAP[port]==NULL || PORT_MAP[port]->type!=SOCKET_LISTENER){
		fprintf(stderr, "%s\n", PORT_MAP[port]->type!=SOCKET_LISTENER ? "PORT_MAP[port]->type!=SOCKET_LISTENER" : "PORT_MAP[port]==NULL");
		return -1;
	}

	// get the socket to connect to
	socket_cb *socket_to_connect_to = PORT_MAP[port];

	// increase refcount
	socket_to_connect_to->refcount++;

	// build the request
	connection_request *request = (connection_request*) xmalloc(sizeof(connection_request));
	request->admitted = 0;
	request->connected_cv = COND_INIT;
	request->peer = cursoc;
	rlnode_init(& request->queue_node, request);

	// add request to the listeners queue and signal listener
	rlist_push_back(& socket_to_connect_to->listener_s.queue, & request->queue_node);
	kernel_signal(& socket_to_connect_to->listener_s.req_available);
	//fprintf(stderr, "request signaled 0x%X\n", & socket_to_connect_to->listener_s.req_available);
	//fprintf(stderr, "request gonna wait on 0x%X\n", &request->connected_cv);
	// block for the specified amount of time
	kernel_timedwait(& request->connected_cv, SCHED_USER, timeout);

	// decrease refcount
	socket_to_connect_to->refcount--;

	// TODO free the request (?)
	//fprintf(stderr, "request woke up from 0x%X and request->admited=%d\n", &request->connected_cv, request->admitted);
	return (request->admitted==1) ? 0 : -1;			// if the request was admitted return 0, else -1
}

int sys_ShutDown(Fid_t sock, shutdown_mode how)
{
	/*
	Possible reasons for error: 
	- the file id sock is not legal (a connected socket stream).
	*/
	return -1;

	// check if given fid is legal and corresponding FCB is NULL
	if(sock < 0 || sock > MAX_FILEID-1 || get_fcb(sock)==NULL){
		return -1;
	}

	// get the socket
	socket_cb *socket_to_close = (socket_cb*) get_fcb(sock)->streamobj;

	// check if the socket with the given fid is a connected one
	if(socket_to_close==NULL || socket_to_close->type!=SOCKET_PEER || socket_to_close->peer_s.peer==NULL){
		return -1;
	}

	// close the appropriate pointers depending on the request shutdown mode
	switch(how) {
		case SHUTDOWN_READ:
			// close socket's reader pipe
			pipe_reader_close(socket_to_close->peer_s.read_pipe);
			socket_to_close->peer_s.read_pipe = NULL;
			break;
		case SHUTDOWN_WRITE:
			// close socket's writer_pipe
			pipe_writer_close(socket_to_close->peer_s.write_pipe);
			socket_to_close->peer_s.write_pipe = NULL;
			break;
		case SHUTDOWN_BOTH:
			// close both the reader and the writer pipes
			pipe_writer_close(socket_to_close->peer_s.write_pipe);
			pipe_reader_close(socket_to_close->peer_s.read_pipe);
			socket_to_close->peer_s.read_pipe = NULL;
			socket_to_close->peer_s.write_pipe = NULL;
			break;
			break;
		default:
			return -1;	
	}	

	// TODO free/delete anything?

	return 0;
}

int socket_write(void* socket, const char *buf, unsigned int n){
	
	// get the socket
	socket_cb *socket_to_write = (socket_cb*) socket;

	// check if the socket is NULL or is not a peer
	if(socket_to_write == NULL || socket_to_write->type != SOCKET_PEER){
		return -1;
	}
 	
	// call pipe_write on the appropriate pipe
	return pipe_write(socket_to_write->peer_s.write_pipe, buf, n);
}

int socket_read(void* socket, char *buf, unsigned int n){

	// get the socket
	socket_cb* socket_to_read = (socket_cb*) socket;

 	// check if the socket is NULL or is not a peer
	if(socket_to_read == NULL || socket_to_read->type != SOCKET_PEER){
		return -1;
	}

	// call pipe_read on the appropriate pipe
 	return pipe_read(socket_to_read->peer_s.read_pipe, buf, n);
}

int socket_close(void* socket){

	// get the socket
	socket_cb* socket_to_close  = (socket_cb*) socket;

	// check if the socket is already closed
	if(socket_to_close == NULL){
		return -1;
	}

	// decide what to do depending on the type of socket
	switch(socket_to_close->type){
		case SOCKET_UNBOUND:
			break;
		case SOCKET_LISTENER:
			// uninstall the socket from the port table
			PORT_MAP[socket_to_close->port] = NULL;

			// signal the listener's condvar to wakeup all waiting sockets
			kernel_broadcast(&socket_to_close->listener_s.req_available);
			break;
		case SOCKET_PEER:
			// close the reader and make the connection NULL
			pipe_reader_close(socket_to_close->peer_s.read_pipe);
			socket_to_close->peer_s.read_pipe = NULL;

			// close the writer and make the connection NULL
			pipe_writer_close(socket_to_close->peer_s.write_pipe);
			socket_to_close->peer_s.write_pipe = NULL;
			break;
		default:
			return -1;
	}
	
	// TODO need to decrease refcount?

	if(socket_to_close->refcount == 0){
		free(socket);
	}	

	return 0;
}