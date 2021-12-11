#ifndef __KERNEL_STREAMS_H
#define __KERNEL_STREAMS_H
#include "tinyos.h"
#include "kernel_dev.h"
#define PIPE_BUFFER_SIZE 32768		// between 4 and 16 kB, according to Pipe() syscall's documentation */

/**
    Node of a doubly linked character list.
*/
typedef struct char_list_node {
    struct char_list_node *prev;
    struct char_list_node *next;
    char c;
} c_node;

typedef struct pipe_control_block {
	FCB *reader, *writer;
	CondVar has_space;    				/* For blocking writer if no space is available */
	CondVar has_data;     				/* For blocking reader until data are available */
	c_node* w_position, *r_position;  	/* write, read position in buffer (pointers to c_nodes) */
	c_node* BUFFER;  	/* bounded (cyclic) byte buffer */
	int written_bytes;	/* need to create mutex ?*/
} pipe_cb;

/**
 * @brief Initialize the pipe object (the pipe_cb)
 * 
 * Read and write positions are set to the head of the
 * buffer, written_bytes is set to 0, and buffer is 
 * set to "\0\0\0\0\0\0\0\0\0...""
 * 
 * @return pipe_cb* The newly created pipe
 */
pipe_cb* init_pipe_obj();

/**
 * @brief Get the empty node of the list. If there is none, returns NULL.
 * 
 * @param list      The list to check
 * @return c_node*  The empty node if found, NULL otherwise
 */
c_node* get_empty_node(c_node* list);

/**
    Function to allocate and initialize a doubly linked circular list of
    characters, given a size and a char* (a string). If the string is
    NULL, all chars in the list will be spaces (' ').

    Returns the head of the newly created list.

    Remember to free() the list when done.
*/
c_node* init_list(int size, const char *data);


/**
	@brief Construct and return a pipe.

	A pipe is a one-directional buffer accessed via two file ids,
	one for each end of the buffer. The size of the buffer is 
	implementation-specific, but can be assumed to be between 4 and 16 
	kbytes. 

	Once a pipe is constructed, it remains operational as long as both
	ends are open. If the read end is closed, the write end becomes 
	unusable: calls on @c Write to it return error. On the other hand,
	if the write end is closed, the read end continues to operate until
	the buffer is empty, at which point calls to @c Read return 0.

	@param pipe a pointer to a pipe_t structure for storing the file ids.
	@returns 0 on success, or -1 on error. Possible reasons for error:
		- the available file ids for the process are exhausted.
*/
int sys_Pipe(pipe_t* pipe);

/**
 * @brief Write up to n bytes off buf to pipecb_t.
 * 
 * @param pipecb_t  The pipe to write to.
 * @param buf       The data to be written.
 * @param n         The max amount of bytes to be written.
 * @return int      The amount of bytes written.
 */
int pipe_write(void* pipecb_t, const char *buf, unsigned int n);

/**
 * @brief Transfer up to n bytes from a pipe to buf
 * 
 * @param pipecb_t  The pipe to read from. 
 * @param buf       The buffer to save the data in.
 * @param n         The max amount of bytes to read.
 * @return int      The amount of bytes read. 0 if write end closed and no data in pipe. -1 if pipe or reader NULL
 */
int pipe_read(void* pipecb_t, char *buf, unsigned int n);

/** @brief Close operation.

      Close the stream object, deallocating any resources held by it.
      This function returns 0 is it was successful and -1 if not.
      Although the value in case of failure is passed to the calling process,
      the stream should still be destroyed.

    Possible errors are:
    - There was a I/O runtime problem.
*/
int pipe_writer_close(void* _pipecb);

/** @brief Close operation.

      Close the stream object, deallocating any resources held by it.
      This function returns 0 is it was successful and -1 if not.
      Although the value in case of failure is passed to the calling process,
      the stream should still be destroyed.

    Possible errors are:
    - There was a I/O runtime problem.
*/
int pipe_reader_close(void* _pipecb);

/**
 * No-op function to have as read() in the writer.
 */
int no_op_read(void* pipecb_t, char *buf, unsigned int n);

/**
 * No-op function to have as write() in the reader.
 */
int no_op_write(void* pipecb_t, const char *buf, unsigned int n);






/**
	@file kernel_streams.h
	@brief Support for I/O streams.


	@defgroup streams Streams.
	@ingroup kernel
	@brief Support for I/O streams.

	The stream model of tinyos3 is similar to the Unix model.
	Streams are objects that are shared between processes.
	Streams are accessed by file IDs (similar to file descriptors
	in Unix).

	The streams of each process are held in the file table of the
	PCB of the process. The system calls generally use the API
	of this file to access FCBs: @ref get_fcb, @ref FCB_reserve
	and @ref FCB_unreserve.

	Streams are connected to devices by virtue of a @c file_operations
	object, which provides pointers to device-specific implementations
	for read, write and close.

	@{
*/



/** @brief The file control block.

	A file control block provides a uniform object to the
	system calls, and contains pointers to device-specific
	functions.
 */
typedef struct file_control_block
{
  uint refcount;  			/**< @brief Reference counter. */
  void* streamobj;			/**< @brief The stream object (e.g., a device) */
  file_ops* streamfunc;		/**< @brief The stream implementation methods */
  rlnode freelist_node;		/**< @brief Intrusive list node */
} FCB;



/** 
  @brief Initialization for files and streams.

  This function is called at kernel startup.
 */
void initialize_files();


/**
	@brief Increase the reference count of an fcb 

	@param fcb the fcb whose reference count will be increased
*/
void FCB_incref(FCB* fcb);


/**
	@brief Decrease the reference count of the fcb.

	If the reference count drops to 0, release the FCB, calling the 
	Close method and returning its return value.
	If the reference count is still >0, return 0. 

	@param fcb  the fcb whose reference count is decreased
	@returns if the reference count is still >0, return 0, else return the value returned by the
	     `Close()` operation
*/
int FCB_decref(FCB* fcb);


/** @brief Acquire a number of FCBs and corresponding fids.

   Given an array of fids and an array of pointers to FCBs  of
   size @ num, this function will check is available resources
   in the current process PCB and FCB are available, and if so
   it will fill the two arrays with the appropriate values.
   If not, the state is unchanged (but the array contents
   may have been overwritten).

   If these resources are not needed, the operation can be
   reversed by calling @ref FCB_unreserve.

   @param num the number of resources to reserve.
   @param fid array of size at least `num` of `Fid_t`.
   @param fcb array of size at least `num` of `FCB*`.
   @returns 1 for success and 0 for failure.
*/
int FCB_reserve(size_t num, Fid_t *fid, FCB** fcb);


/** @brief Release a number of FCBs and corresponding fids.

   Given an array of fids of size @ num, this function will 
   return the fids to the free pool of the current process and
   release the corresponding FCBs.

   This is the opposite of operation @ref FCB_reserve. 
   Note that this is very different from closing open fids.
   No I/O operation is performed by this function.

   This function does not check its arguments for correctness.
   Use only with arrays filled by a call to @ref FCB_reserve.

   @param num the number of resources to unreserve.
   @param fid array of size at least `num` of `Fid_t`.
   @param fcb array of size at least `num` of `FCB*`.
*/
void FCB_unreserve(size_t num, Fid_t *fid, FCB** fcb);


/** @brief Translate an fid to an FCB.

	This routine will return NULL if the fid is not legal.

	@param fid the file ID to translate to a pointer to FCB
	@returns a pointer to the corresponding FCB, or NULL.
 */
FCB* get_fcb(Fid_t fid);


/** @} */

#endif
