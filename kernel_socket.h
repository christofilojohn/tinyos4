#include "bios.h"
#include "tinyos.h"
#include "kernel_streams.h"
#include "util.h"
#include "kernel_dev.h"


/**
 * Possible socket states. 
*/
typedef enum SOCKET_TYPE {
	SOCKET_LISTENER,    /** A listener socket waiting to serve connect requests. */
    SOCKET_UNBOUND,     /** An unbound socket. This is the type of the socket that Socket() returns */
    SOCKET_PEER         /** A peer socket connected with another one. */
} socket_type;


/**
 * Listener socket (waiting for inbound connections)
*/
typedef struct listener_socket{
    rlnode queue;           /** The queue in which requests to connect with this socket will be stored */
    CondVar req_available;  /** The conditional variable that incoming requests will signal to wake the listener up */
} listener_socket;


/**
 * Unbound socket (not a listener or a peer yet)
*/
typedef struct unbound_socket{
    rlnode socket_node;     /** The node through which the socket will be attached to queues */
} unbound_socket;


/**
 * Peer socket (connected with another one and able to "talk")
*/
typedef struct peer_socket{
    struct peer_socket* peer;        /** The socket this one is connected to */
    pipe_cb* write_pipe;    /** The pipe to which this socket writes */
    pipe_cb* read_pipe;     /** The pipe from which this socket reads */
} peer_socket;


/**
 * @brief The socket control block
 * 
 * A socket control block represents a socket. It contains its
 * refcount, the FCB to which it is bound, its type and the
 * rest of the fields according to the type.
*/
typedef struct socket_control_block {
    uint refcount;          /** The counter of references to this socket. When 0, we can safely delete it. */
    FCB* fcb;               /** The FCB through which this socket is accessible (its file descriptor) for backwards connectivity. Can be used as socket state indicator (when NULL, the sockets is closed) */
    socket_type type;       /** The type of this socket. The type of the socket matches the union member with the rest of the data */
	port_t port;			/** The port that the socket is bound to */
    union {
        listener_socket listener_s;     /** The listener socket object */
        unbound_socket unbound_s;       /** The unbound socket object. This could be ommited but is here for the sake of uniformity*/
        peer_socket peer_s;             /** The peer socket object */
    };
} socket_cb;


/**
 * @brief The connection request object.
 * 
 * A connection request is the way sockets connect to a listener.
 * A socket that wants to become peers with another one (which is 
 * listening on a port) sends a connection request (through Connect())
 * to it. The request is then served by the listener and a connection
 * is established.
*/
typedef struct connection_request {
    int admitted;           /** A flag that shows whether the request was served (if 1, a connection was successfully established.)  */
    socket_cb* peer;        /** The socket that makes the request */
    CondVar connected_cv;   /** The conditional variable on which the connecting socket sleeps until the request is served or the timeout time has passed. */
    rlnode queue_node;      /** The node through which the request is attached to the listeners request queue */
} connection_request;


/**
 * Allocate and initialize the fields of a socket. Returns the newly created socket.
*/
socket_cb* init_socket_cb(FCB* parent_fcb);


/**
 * @brief Write up to n bytes off buf to pipecb_t.
 * 
 * @param pipecb_t  The pipe to write to.
 * @param buf       The data to be written.
 * @param n         The max amount of bytes to be written.
 * @return int      The amount of bytes written.
 */
int socket_write(void* socket, const char *buf, unsigned int n);


/**
 * @brief Transfer up to n bytes from a pipe to buf
 * 
 * @param pipecb_t  The pipe to read from. 
 * @param buf       The buffer to save the data in.
 * @param n         The max amount of bytes to read.
 * @return int      The amount of bytes read. 0 if write end closed and no data in pipe. -1 if pipe or reader NULL
 */
int socket_read(void* socket, char *buf, unsigned int n);


/** @brief Close operation.

      Close the stream object, deallocating any resources held by it.
      This function returns 0 is it was successful and -1 if not.
      Although the value in case of failure is passed to the calling process,
      the stream should still be destroyed.

    Possible errors are:
    - There was a I/O runtime problem.
*/
int socket_close(void* socket);


/**
	@brief Return a new socket bound on a port.

	This function returns a file descriptor for a new
	socket object.	If the @c port argument is NOPORT, then the 
	socket will not be bound to a port. Else, the socket
	will be bound to the specified port. 

	@param port the port the new socket will be bound to
	@returns a file id for the new socket, or NOFILE on error. Possible
		reasons for error:
		- the port is iilegal
		- the available file ids for the process are exhausted
*/
Fid_t sys_Socket(port_t port);


/**
	@brief Initialize a socket as a listening socket.

	A listening socket is one which can be passed as an argument to
	@c Accept. Once a socket becomes a listening socket, it is not
	possible to call any other functions on it except @c Accept, @Close
	and @c Dup2().

	The socket must be bound to a port, as a result of calling @c Socket.
	On each port there must be a unique listening socket (although any number
	of non-listening sockets are allowed).

	@param sock the socket to initialize as a listening socket
	@returns 0 on success, -1 on error. Possible reasons for error:
		- the file id is not legal
		- the socket is not bound to a port
		- the port bound to the socket is occupied by another listener
		- the socket has already been initialized
	@see Socket
 */
int sys_Listen(Fid_t sock);


/**
	@brief Wait for a connection.

	With a listening socket as its sole argument, this call will block waiting
	for a single @c Connect() request on the socket's port. 
	one which can be passed as an argument to @c Accept. 

	It is possible (and desirable) to re-use the listening socket in multiple successive
	calls to Accept. This is a typical pattern: a thread blocks at Accept in a tight
	loop, where each iteration creates new a connection, 
	and then some thread takes over the connection for communication with the client.

	@param sock the socket to initialize as a listening socket
	@returns a new socket file id on success, @c NOFILE on error. Possible reasons 
	    for error:
		- the file id is not legal
		- the file id is not initialized by @c Listen()
		- the available file ids for the process are exhausted
		- while waiting, the listening socket @c lsock was closed

	@see Connect
	@see Listen
 */
Fid_t sys_Accept(Fid_t lsock);


/**
	@brief Create a connection to a listener at a specific port.

	Given a socket @c sock and @c port, this call will attempt to establish
	a connection to a listening socket on that port. If sucessful, the
	@c sock stream is connected to the new stream created by the listener.

	The two connected sockets communicate by virtue of two pipes of opposite directions, 
	but with one file descriptor servicing both pipes at each end.

	The connect call will block for approximately the specified amount of time.
	The resolution of this timeout is implementation specific, but should be
	in the order of 100's of msec. Therefore, a timeout of at least 500 msec is
	reasonable. If a negative timeout is given, it means, "infinite timeout".

	@params sock the socket to connect to the other end
	@params port the port on which to seek a listening socket
	@params timeout the approximate amount of time to wait for a
	        connection.
	@returns 0 on success and -1 on error. Possible reasons for error:
	   - the file id @c sock is not legal (i.e., an unconnected, non-listening socket)
	   - the given port is illegal.
	   - the port does not have a listening socket bound to it by @c Listen.
	   - the timeout has expired without a successful connection.
*/
int sys_Connect(Fid_t sock, port_t port, timeout_t timeout);


/**
   @brief Shut down one direction of socket communication.

   With a socket which is connected to another socket, this call will 
   shut down one or the other direction of communication. The shut down
   of a direction has implications similar to those of a pipe's end shutdown.
   More specifically, assume that this end is socket A, connected to socket
   B at the other end. Then,

   - if `ShutDown(A, SHUTDOWN_READ)` is called, any attempt to call `Write(B,...)`
     will fail with a code of -1.
   - if ShutDown(A, SHUTDOWN_WRITE)` is called, any attempt to call `Read(B,...)`
     will first exhaust the buffered data and then will return 0.
   - if ShutDown(A, SHUTDOWN_BOTH)` is called, it is equivalent to shutting down
     both read and write.

   After shutdown of socket A, the corresponding operation `Read(A,...)` or `Write(A,...)`
   will return -1.

   Shutting down multiple times is not an error.
   
   @param sock the file ID of the socket to shut down.
   @param how the type of shutdown requested
   @returns 0 on success and -1 on error. Possible reasons for error:
       - the file id @c sock is not legal (a connected socket stream).
*/
int sys_ShutDown(Fid_t sock, shutdown_mode how);

