/*
 * Copyright (c) 2022-2023 NVIDIA CORPORATION & AFFILIATES, ALL RIGHTS RESERVED.
 *
 * This software product is a proprietary product of NVIDIA CORPORATION &
 * AFFILIATES (the "Company") and all right, title, and interest in and to the
 * software product, including all associated intellectual property rights, are
 * and shall remain exclusively with the Company.
 *
 * This software product is governed by the End User License Agreement
 * provided with the software product.
 *
 */

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include <zlib.h>
#include <zstd.h>

#include <doca_argp.h>
#include <doca_log.h>

#include <pack.h>
#include <utils.h>
#include <doca_utils.h>

#include "file_compression_total_core.h"

#define MAX_MSG 512				/* Maximum number of messages in CC queue */
#define SLEEP_IN_NANOS (1 * 1000)		/* Sample the job every 10 microseconds */
#define PAGE_SIZE (sysconf(_SC_PAGESIZE))	/* OS page size */
#define MAX_FILE_SIZE (128 * 1024 * 1024)	/* 128 Mbytes */
#define DECOMPRESS_RATIO 1032			/* Maximal decompress ratio size */
#define DEFAULT_TIMEOUT 10			/* default timeout for receiving messages */
#define SERVER_NAME "file_compression_server"	/* CC server name */

#define CHECK_ERR(...) \
if (result != DOCA_SUCCESS) {\
	DOCA_DLOG_ERR(__VA_ARGS__);\
	return result;\
}\

DOCA_LOG_REGISTER(FILE_COMPRESSION::Core);

/*
 * Set Comm Channel properties
 *
 * @mode [in]: Running mode
 * @ep [in]: DOCA comm_channel endpoint
 * @dev [in]: DOCA device object to use
 * @dev_rep [in]: DOCA device representor object to use
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t
set_endpoint_properties(enum file_compression_mode mode, struct doca_comm_channel_ep_t *ep, struct doca_dev *dev, struct doca_dev_rep *dev_rep)
{
	doca_error_t result;

	result = doca_comm_channel_ep_set_device(ep, dev);
	CHECK_ERR("Failed to set DOCA device property")

	result = doca_comm_channel_ep_set_max_msg_size(ep, MAX_MSG_SIZE);
	CHECK_ERR("Failed to set max_msg_size property")

	result = doca_comm_channel_ep_set_send_queue_size(ep, MAX_MSG);
	CHECK_ERR("Failed to set snd_queue_size property")

	result = doca_comm_channel_ep_set_recv_queue_size(ep, MAX_MSG);
	CHECK_ERR("Failed to set rcv_queue_size property")

	if (mode == SERVER) {
		result = doca_comm_channel_ep_set_device_rep(ep, dev_rep);
		CHECK_ERR("Failed to set DOCA device representor property")
	}
	return DOCA_SUCCESS;
}

/*
 * Unmap callback - free doca_buf allocated pointer
 *
 * @addr [in]: Memory range pointer
 * @len [in]: Memory range length
 * @opaque [in]: An opaque pointer passed to iterator
 */
static void
unmap_cb(void *addr, size_t len, void *opaque)
{
	(void)opaque;

	if (addr != NULL)
		munmap(addr, len);
}

/*
 * Submit compress job and retrieve the result
 *
 * @state [in]: application configuration struct
 * @job [in]: job to submit
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t
process_job(struct program_core_objects *state, const struct doca_job *job)
{
	struct doca_event event = {0};
	struct timespec ts;
	doca_error_t result;

	/* Enqueue job */
	result = doca_workq_submit(state->workq, job);
	CHECK_ERR("Failed to submit doca job: %s", doca_get_error_string(result))

	/* Wait for job completion */
	while ((result = doca_workq_progress_retrieve(state->workq, &event, DOCA_WORKQ_RETRIEVE_FLAGS_NONE)) ==
	       DOCA_ERROR_AGAIN) {
		/* Wait for the job to complete */
		ts.tv_sec = 0;
		ts.tv_nsec = SLEEP_IN_NANOS;
		nanosleep(&ts, &ts);
	}

	if (result != DOCA_SUCCESS)
		DOCA_DLOG_ERR("Failed to retrieve job: %s", doca_get_error_string(result));
	else if (event.result.u64 != DOCA_SUCCESS) {
		DOCA_DLOG_ERR("Job finished unsuccessfully");
		result = event.result.u64;
	} else
		result = DOCA_SUCCESS;

	return result;
}

/*
 * Populate destination doca buffer for compress jobs
 *
 * @state [in]: application configuration struct
 * @dst_buffer [in]: destination buffer
 * @dst_buf_size [in]: destination buffer size
 * @dst_doca_buf [out]: created doca buffer
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t
populate_dst_buf(struct program_core_objects *state, uint8_t **dst_buffer, size_t dst_buf_size, struct doca_buf **dst_doca_buf)
{
	doca_error_t result;

	dst_buffer = calloc(1, dst_buf_size);
	if (dst_buffer == NULL) {
		DOCA_DLOG_ERR("Failed to allocate memory");
		return DOCA_ERROR_NO_MEMORY;
	}

	result = doca_mmap_set_memrange(state->dst_mmap, dst_buffer, dst_buf_size);
	if (result != DOCA_SUCCESS) {
		DOCA_DLOG_ERR("Unable to set memory range destination memory map: %s", doca_get_error_string(result));
		free(dst_buffer);
		return result;
	}

	result = doca_mmap_start(state->dst_mmap);
	if (result != DOCA_SUCCESS) {
		DOCA_DLOG_ERR("Unable to start destination memory map: %s", doca_get_error_string(result));
		free(dst_buffer);
		return result;
	}

	result = doca_buf_inventory_buf_by_addr(state->buf_inv, state->dst_mmap, dst_buffer, dst_buf_size,
						dst_doca_buf);
	CHECK_ERR("Unable to acquire DOCA buffer representing destination buffer: %s", doca_get_error_string(result))
	return result;
}

/*
 * Construct compress job and submit it
 *
 * @state [in]: application configuration struct
 * @file_data [in]: file data to the source buffer
 * @file_size [in]: file size
 * @job_type [in]: compress job type - compress for client and decompress for server
 * @dst_buf_size [in]: allocated destination buffer length
 * @compressed_file [out]: destination buffer with the result
 * @compressed_file_len [out]: destination buffer size
 * @output_chksum [out]: the returned checksum
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t
compress_file_hw(struct program_core_objects *state, char *file_data, size_t file_size,
	      enum doca_compress_job_types job_type, size_t dst_buf_size, uint8_t **compressed_file,
	      size_t *compressed_file_len, uint64_t *output_chksum)
{
	struct doca_buf *dst_doca_buf;
	struct doca_buf *src_doca_buf;
	uint8_t *resp_head;
	doca_error_t result;

	result = doca_mmap_set_memrange(state->src_mmap, file_data, file_size);
	if (result != DOCA_SUCCESS) {
		DOCA_DLOG_ERR("Unable to set memory range of source memory map: %s", doca_get_error_string(result));
		munmap(file_data, file_size);
		return result;
	}
	result = doca_mmap_set_free_cb(state->src_mmap, &unmap_cb, NULL);
	if (result != DOCA_SUCCESS) {
		DOCA_DLOG_ERR("Unable to set free callback of source memory map: %s", doca_get_error_string(result));
		munmap(file_data, file_size);
		return result;
	}
	result = doca_mmap_start(state->src_mmap);
	if (result != DOCA_SUCCESS) {
		DOCA_DLOG_ERR("Unable to start source memory map: %s", doca_get_error_string(result));
		munmap(file_data, file_size);
		return result;
	}

	result = doca_buf_inventory_buf_by_addr(state->buf_inv, state->src_mmap, file_data, file_size, &src_doca_buf);
	if (result != DOCA_SUCCESS) {
		DOCA_DLOG_ERR("Unable to acquire DOCA buffer representing source buffer: %s", doca_get_error_string(result));
		return result;
	}

	doca_buf_get_data(src_doca_buf, (void **)&resp_head);
	doca_buf_set_data(src_doca_buf, resp_head, file_size);

	result = populate_dst_buf(state, compressed_file, dst_buf_size, &dst_doca_buf);
	if (result != DOCA_SUCCESS) {
		doca_buf_refcount_rm(src_doca_buf, NULL);
		return result;
	}

	/* Construct compress job */
	const struct doca_compress_deflate_job compress_job = {
		.base = (struct doca_job) {
			.type = job_type,
			.flags = DOCA_JOB_FLAGS_NONE,
			.ctx = state->ctx,
			},
		.dst_buff = dst_doca_buf,
		.src_buff = src_doca_buf,
		.output_chksum = output_chksum,
	};

	result = process_job(state, &compress_job.base);
	if (result != DOCA_SUCCESS) {
		doca_buf_refcount_rm(dst_doca_buf, NULL);
		doca_buf_refcount_rm(src_doca_buf, NULL);
		return result;
	}

	doca_buf_refcount_rm(src_doca_buf, NULL);

	doca_buf_get_head(dst_doca_buf, (void **)compressed_file);
	doca_buf_get_data_len(dst_doca_buf, compressed_file_len);
	doca_buf_refcount_rm(dst_doca_buf, NULL);

	return DOCA_SUCCESS;
}

/*
 * Compress the input file in SW
 *
 * @file_data [in]: file data to the source buffer
 * @file_size [in]: file size
 * @dst_buf_size [in]: allocated destination buffer length
 * @compressed_file [out]: destination buffer with the result
 * @compressed_file_len [out]: destination buffer size
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t
compress_file_sw(char *file_data, size_t file_size, size_t dst_buf_size, Byte **compressed_file, uLong *compressed_file_len)
{
	z_stream c_stream; /* compression stream */
	int err;

	memset(&c_stream, 0, sizeof(c_stream));

	c_stream.zalloc = NULL;
	c_stream.zfree = NULL;

	err = deflateInit2(&c_stream, Z_DEFAULT_COMPRESSION, Z_DEFLATED, -MAX_WBITS, MAX_MEM_LEVEL, Z_DEFAULT_STRATEGY);
	c_stream.next_in  = (z_const unsigned char *)file_data;
	c_stream.next_out = *compressed_file;

	c_stream.avail_in = file_size;
	c_stream.avail_out = dst_buf_size;
	err = deflate(&c_stream, Z_NO_FLUSH);
	if (err < 0) {
		DOCA_DLOG_ERR("Failed to compress file");
		return DOCA_ERROR_BAD_STATE;
	}

	/* Finish the stream */
	err = deflate(&c_stream, Z_FINISH);
	if (err < 0 || err != Z_STREAM_END) {
		DOCA_DLOG_ERR("Failed to compress file");
		return DOCA_ERROR_BAD_STATE;
	}

	err = deflateEnd(&c_stream);
	if (err < 0) {
		DOCA_DLOG_ERR("Failed to compress file");
		return DOCA_ERROR_BAD_STATE;
	}
	*compressed_file_len = c_stream.total_out;
	return DOCA_SUCCESS;
}



/*
 * Compress / decompress the input file data
 *
 * @state [in]: application configuration struct
 * @file_data [in]: file data to the source buffer
 * @file_size [in]: file size
 * @job_type [in]: compress job type - compress for client and decompress for server
 * @compress_method [in]: compress with software or hardware
 * @compressed_file [out]: destination buffer with the result
 * @compressed_file_len [out]: destination buffer size
 * @output_chksum [out]: the calculated checksum
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t
compress_file(struct program_core_objects *state, char *file_data, size_t file_size, enum doca_compress_job_types job_type,
	      enum file_compression_compress_method compress_method, uint8_t **compressed_file, size_t *compressed_file_len,
	      uint64_t *output_chksum)
{
	size_t dst_buf_size = 0;

	if (job_type == DOCA_COMPRESS_DEFLATE_JOB) {
		dst_buf_size = MAX(file_size + 16, file_size * 2);
		if (dst_buf_size > MAX_FILE_SIZE)
			dst_buf_size = MAX_FILE_SIZE;
	} else if (job_type == DOCA_DECOMPRESS_DEFLATE_JOB)
		dst_buf_size = MIN(MAX_FILE_SIZE, DECOMPRESS_RATIO * file_size);

	*compressed_file = calloc(1, dst_buf_size);
	if (*compressed_file == NULL) {
		DOCA_DLOG_ERR("Failed to allocate memory");
		return DOCA_ERROR_NO_MEMORY;
	}

	if (compress_method == COMPRESS_DEFLATE_SW) {
		// calculate_checksum_sw(file_data, file_size, output_chksum);
		return compress_file_sw(file_data, file_size, dst_buf_size, compressed_file, compressed_file_len);
	} else { // if (compress_method == COMPRESS_DEFLATE_HW) {
		return compress_file_hw(state, file_data, file_size, job_type, dst_buf_size, compressed_file, compressed_file_len, output_chksum);
	}
}

/*
 * Send the input data with comm channel to a remote peer
 *
 * @ep [in]: handle for comm channel local endpoint
 * @peer_addr [in]: destination address handle of the send operation
 * @buf [in]: data to the source buffer
 * @buf_size [in]: data size
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t
send_data(struct doca_comm_channel_ep_t *ep, struct doca_comm_channel_addr_t **peer_addr,
	 char *buf, size_t buf_size)
{
	size_t total_chunks;
	size_t chunk_len;
	uint32_t i;
	doca_error_t result;
	struct timespec ts = {
		.tv_nsec = SLEEP_IN_NANOS,
	};

	/* Send the number of chunks  */
	total_chunks = (buf_size + MAX_MSG_SIZE - 1) / MAX_MSG_SIZE;
	DOCA_DLOG_DBG("total chunks %zu", total_chunks);
	while ((result = doca_comm_channel_ep_sendto(ep, &total_chunks, sizeof(uint32_t), DOCA_CC_MSG_FLAG_NONE,
						     *peer_addr)) == DOCA_ERROR_AGAIN)
		nanosleep(&ts, &ts);
	CHECK_ERR("Message was not sent: %s", doca_get_error_string(result))


	/* Send file to the server */
	for (i = 0; i < total_chunks; i++) {
		chunk_len = MIN(buf_size, MAX_MSG_SIZE);
		while ((result = doca_comm_channel_ep_sendto(ep, buf, chunk_len, DOCA_CC_MSG_FLAG_NONE, *peer_addr)) == DOCA_ERROR_AGAIN)
			nanosleep(&ts, &ts);
		CHECK_ERR("Message was not sent: %s", doca_get_error_string(result))
		buf += chunk_len;
		buf_size -= chunk_len;
	}

	/* Wait for ACK message */
	char ack[1];
	size_t ack_len = 1;
	while((result = doca_comm_channel_ep_recvfrom(ep, ack, &ack_len, DOCA_CC_MSG_FLAG_NONE,
							peer_addr) == DOCA_ERROR_AGAIN))
		nanosleep(&ts, &ts);
	CHECK_ERR("ACK not received")

	return DOCA_SUCCESS;
}


/*
 * Receive file data with comm channel from a remote peer
 *
 * @ep [in]: handle for comm channel local endpoint
 * @peer_addr [in]: destination address handle of the send operation
 * @buf [out]: pointer to received file data. Buffer is allocated in this function
 * @buf_size [out]: file size
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t
recv_data(struct doca_comm_channel_ep_t *ep, 
		struct doca_comm_channel_addr_t **peer_addr,
		char **buf,
		size_t *buf_size,
		struct file_compression_config *app_cfg)
{
	doca_error_t result;
	uint32_t total_chunks;
	size_t total_chunks_msg_len;
	struct timespec ts = {
		.tv_nsec = SLEEP_IN_NANOS,
	};
	int num_of_iterations = (app_cfg->timeout * 1000 * 1000) / (SLEEP_IN_NANOS / 1000);
	int counter;
	char *buf_ptr;
	size_t chunk_len;


	// Receive total number of chunks (total_chunks) from a remote peer
	counter = 0;
	char total_chunks_msg[4] = {0};
	total_chunks_msg_len = MAX_MSG_SIZE;
	while((result = doca_comm_channel_ep_recvfrom(ep, total_chunks_msg, &total_chunks_msg_len, DOCA_CC_MSG_FLAG_NONE,
							peer_addr) == DOCA_ERROR_AGAIN)) {
		total_chunks_msg_len = MAX_MSG_SIZE;
		nanosleep(&ts, &ts);
		if ((counter++) == num_of_iterations) {
			DOCA_DLOG_ERR("total_chunks message was not received at the given timeout");
			return result;
		}
	}
	CHECK_ERR("total_chunks message was not received: %s", doca_get_error_string(result))
	if (total_chunks_msg_len != sizeof(uint32_t)) {
			DOCA_DLOG_ERR("Received wrong message size, required %ld, got %ld", sizeof(uint32_t), total_chunks_msg_len);
			return DOCA_ERROR_UNEXPECTED;
	}

	// Allocate buffer for receiving data
	total_chunks = *(uint32_t *)total_chunks_msg;
	DOCA_DLOG_DBG("total_chunks=%"PRIx32"", total_chunks);
	DOCA_DLOG_DBG("total_chunks_msg_len=%ld", total_chunks_msg_len);

	*buf_size = MIN(total_chunks * MAX_MSG_SIZE, MAX_FILE_SIZE);
	
	*buf = (char *)calloc(1, *buf_size);
	if (buf == NULL) {
		DOCA_DLOG_ERR("Failed to allocate memory");
		return DOCA_ERROR_NO_MEMORY;
	}

	
	// Receive data (buf) from a remote peer
	buf_ptr = *buf;
	char chunk[MAX_MSG_SIZE] = {0};
	for(uint32_t i=0; i < total_chunks; i++) {
		memset(chunk, 0, sizeof(chunk));
		counter = 0;
		chunk_len = MAX_MSG_SIZE;
		while((result = doca_comm_channel_ep_recvfrom(ep, chunk, &chunk_len, DOCA_CC_MSG_FLAG_NONE,
							peer_addr) == DOCA_ERROR_AGAIN)) {
			chunk_len = MAX_MSG_SIZE;
			nanosleep(&ts, &ts);
			counter++;
			if (counter == num_of_iterations) {
				DOCA_DLOG_ERR("Message was not received at the given timeout");
				return result;
			}
		}
		CHECK_ERR("Message was not received: %s", doca_get_error_string(result))

		if (buf_ptr - *buf + MAX_MSG_SIZE > MAX_FILE_SIZE) {
			DOCA_DLOG_ERR("Received file exceeded maximum file size");
			return DOCA_ERROR_UNEXPECTED;
		}
		memcpy(buf_ptr, chunk, chunk_len);
		buf_ptr += chunk_len;
	}

	/* Send ACK message */
	char ack[1] = "A";
	while ((result = doca_comm_channel_ep_sendto(ep, ack, 1, DOCA_CC_MSG_FLAG_NONE, *peer_addr)) == DOCA_ERROR_AGAIN)
			nanosleep(&ts, &ts);
	CHECK_ERR("Message was not sent: %s", doca_get_error_string(result))

	return DOCA_SUCCESS;
}


/*
 * This function measures the compression time from data retrieval from DPU memory to completion of compression.
 * - HW deflate
*/
static doca_error_t
compression_experiment_on_bluefield(struct program_core_objects *state, char* received_file, size_t received_file_size, uint8_t **compressed_file, size_t *compressed_file_len)
{
	uint64_t checksum;
	doca_error_t result;
	uint8_t *compressed_f;
	size_t compressed_f_len;

	result = compress_file(state, received_file, received_file_size, DOCA_COMPRESS_DEFLATE_JOB, COMPRESS_DEFLATE_HW, &compressed_f, &compressed_f_len, &checksum);

	*compressed_file = compressed_f;
	*compressed_file_len = compressed_f_len;

	return result;
}

doca_error_t
file_compression_client(struct doca_comm_channel_ep_t *ep, struct doca_comm_channel_addr_t **peer_addr,
		      struct file_compression_config *app_cfg, struct program_core_objects *state)
{
	int fd;
	struct stat statbuf;
	char *file_data;
	doca_error_t result;


	fd = open(app_cfg->file_path, O_RDWR);
	if (fd < 0) {
		DOCA_DLOG_ERR("Failed to open %s", app_cfg->file_path);
		return DOCA_ERROR_IO_FAILED;
	}

	if (fstat(fd, &statbuf) < 0) {
		DOCA_DLOG_ERR("Failed to get file information");
		close(fd);
		return DOCA_ERROR_IO_FAILED;
	}

	if (statbuf.st_size == 0 || statbuf.st_size > MAX_FILE_SIZE) {
		DOCA_DLOG_ERR("Invalid file size. Should be greater then zero and smaller then two Gbytes");
		close(fd);
		return DOCA_ERROR_INVALID_VALUE;
	}

	file_data = mmap(NULL, statbuf.st_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (file_data == MAP_FAILED) {
		DOCA_DLOG_ERR("Unable to map file content: %s", strerror(errno));
		close(fd);
		return DOCA_ERROR_NO_MEMORY;
	}
	DOCA_DLOG_INFO("File size: %ld", statbuf.st_size);
	close(fd);

	// step1. timer start
	struct timeval start_time, cur_time;
	double sec, micro, tat;
	gettimeofday(&start_time, NULL);
	// 2. send file data len & file data
	result = send_data(ep, peer_addr, file_data, statbuf.st_size);
	CHECK_ERR("Failed to send data")
	// 3. invoke recv function to wait for compressed data.
	char *recv_buf;
	size_t recv_buf_len;
	result = recv_data(ep, peer_addr, &recv_buf, &recv_buf_len, app_cfg);
	CHECK_ERR("Failed to receive data")

	// 4. timer stop
	gettimeofday(&cur_time, NULL);
	sec = (double)(cur_time.tv_sec - start_time.tv_sec);
	micro = (double)(cur_time.tv_usec - start_time.tv_usec);
	tat = sec + micro / 1000.0 / 1000.0;
	DOCA_DLOG_DBG("[CLIENT] BF2 Deflate TAT time %lf (%ld->%ld)", tat, statbuf.st_size, recv_buf_len);

	DOCA_DLOG_DBG("[CLIENT] Finish Client");
	return DOCA_SUCCESS;
}

doca_error_t
file_compression_server(struct doca_comm_channel_ep_t *ep, struct doca_comm_channel_addr_t **peer_addr,
		struct file_compression_config *app_cfg, struct program_core_objects *state)
{
	doca_error_t result;
	char *recv_buf;
	size_t recv_buf_len;
	char *send_buf;
	size_t send_buf_len;

	DOCA_DLOG_INFO("[SERVER] Waiting for data to compress");
	// step1. invoke recv function and wait for file data len & file data
	result = recv_data(ep, peer_addr, &recv_buf, &recv_buf_len, app_cfg);
	CHECK_ERR("[SERVER] recv_data failed")

	// 2. compress file data by HW deflate
	result = compression_experiment_on_bluefield(state, recv_buf, recv_buf_len, (uint8_t **)&send_buf, &send_buf_len);
	CHECK_ERR("[SERVER] Failed to compress data")

	// 3. send compressed data to client
	result = send_data(ep, peer_addr, send_buf, send_buf_len);

	DOCA_DLOG_INFO("[SERVER] Finish Server...");
	return result;
}




/* 
 * =============================================================
 * The following sections contain argument parser functions 
 * and functions that perform initialization of the communication channel.
 * 
 * !!Not related to client-server communication.
 * =============================================================
*/

/**
 * Check if given device is capable of executing a DOCA_COMPRESS_DEFLATE_JOB.
 *
 * @devinfo [in]: The DOCA device information
 * @return: DOCA_SUCCESS if the device supports DOCA_COMPRESS_DEFLATE_JOB and DOCA_ERROR otherwise.
 */
static doca_error_t
compress_jobs_compress_is_supported(struct doca_devinfo *devinfo)
{
	return doca_compress_job_get_supported(devinfo, DOCA_COMPRESS_DEFLATE_JOB);
}

/**
 * Check if given device is capable of executing a DOCA_DECOMPRESS_DEFLATE_JOB.
 *
 * @devinfo [in]: The DOCA device information
 * @return: DOCA_SUCCESS if the device supports DOCA_DECOMPRESS_DEFLATE_JOB and DOCA_ERROR otherwise.
 */
static doca_error_t
compress_jobs_decompress_is_supported(struct doca_devinfo *devinfo)
{
	return doca_compress_job_get_supported(devinfo, DOCA_DECOMPRESS_DEFLATE_JOB);
}

doca_error_t
file_compression_init(struct doca_comm_channel_ep_t **ep, struct doca_comm_channel_addr_t **peer_addr,
		struct file_compression_config *app_cfg, struct program_core_objects *state,
		struct doca_compress **compress_ctx)
{
	struct doca_pci_bdf pci_bdf;
	struct doca_dev *cc_doca_dev;
	struct doca_dev_rep *cc_doca_dev_rep = NULL;
	uint32_t workq_depth = 1; /* The app will run 1 compress job at a time */
	uint32_t max_bufs = 2;    /* The app will use 2 doca buffers */
	doca_error_t result;
	struct timespec ts = {
		.tv_nsec = SLEEP_IN_NANOS,
	};

	app_cfg->compress_method = COMPRESS_DEFLATE_HW;

	/* set default timeout */
	if (app_cfg->timeout == 0)
		app_cfg->timeout = DEFAULT_TIMEOUT;

	/* Create Comm Channel endpoint */
	result = doca_comm_channel_ep_create(ep); // 参照：https://docs.nvidia.com/doca/sdk/comm-channel-programming-guide/graphics/establishing-connection.png
	CHECK_ERR("Failed to create Comm Channel endpoint: %s", doca_get_error_string(result))

	/* create compress library */
	result = doca_compress_create(compress_ctx);
	if (result != DOCA_SUCCESS) {
		doca_comm_channel_ep_destroy(*ep);
		DOCA_DLOG_ERR("Failed to init compress library: %s", doca_get_error_string(result));
		return result;
	}

	state->ctx = doca_compress_as_ctx(*compress_ctx);

	result = doca_pci_bdf_from_string(app_cfg->cc_dev_pci_addr, &pci_bdf);
	if (result != DOCA_SUCCESS) {
		DOCA_DLOG_ERR("Invalid PCI address: %s", doca_get_error_string(result));
		goto compress_destroy;
	}

	/* open DOCA device for CC */
	result = open_doca_device_with_pci(&pci_bdf, NULL, &cc_doca_dev);
	if (result != DOCA_SUCCESS) {
		DOCA_DLOG_ERR("Failed to open DOCA device: %s", doca_get_error_string(result));
		goto compress_destroy;
	}

	/* open representor device for CC server */
	if (app_cfg->mode == SERVER) {
		result = doca_pci_bdf_from_string(app_cfg->cc_dev_rep_pci_addr, &pci_bdf);
		if (result != DOCA_SUCCESS) {
			DOCA_DLOG_ERR("Invalid PCI address: %s", doca_get_error_string(result));
			goto dev_close;
		}

		result = open_doca_device_rep_with_pci(cc_doca_dev, DOCA_DEV_REP_FILTER_NET, &pci_bdf, &cc_doca_dev_rep);
		if (result != DOCA_SUCCESS) {
			DOCA_DLOG_ERR("Failed to open representor device: %s", doca_get_error_string(result));
			goto dev_close;
		}
	}

	/* open device for compress job */
	if (app_cfg->mode == SERVER) {
		// ==== server code =====
		result = open_doca_device_with_capabilities(&compress_jobs_decompress_is_supported, &state->dev);
		if (result != DOCA_SUCCESS) {
			DOCA_DLOG_ERR("Failed to open DOCA device for DOCA Compress: %s", doca_get_error_string(result));
			goto rep_dev_close;
		}
	} else {
		// ==== client code =====
		result = open_doca_device_with_capabilities(&compress_jobs_compress_is_supported, &state->dev);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_WARN("Failed to find device for compress job, running SW compress with zlib");
			app_cfg->compress_method = COMPRESS_DEFLATE_SW;
		}
	}

	/* Set ep attributes */
	result = set_endpoint_properties(app_cfg->mode, *ep, cc_doca_dev, cc_doca_dev_rep);
	if (result != DOCA_SUCCESS) {
		DOCA_DLOG_ERR("Failed to init DOCA core objects: %s", doca_get_error_string(result));
		goto rep_dev_close;
	}

	if (app_cfg->compress_method == COMPRESS_DEFLATE_HW) {
		result = init_core_objects(state, workq_depth, max_bufs);
		if (result != DOCA_SUCCESS) {
			DOCA_DLOG_ERR("Failed to init DOCA core objects: %s", doca_get_error_string(result));
			goto destroy_core_objs;
		}
	}

	if (app_cfg->mode == CLIENT) {
		// ==== client code =====
		result = doca_comm_channel_ep_connect(*ep, SERVER_NAME, peer_addr);
		if (result != DOCA_SUCCESS) {
			DOCA_DLOG_ERR("Couldn't establish a connection with the server node: %s", doca_get_error_string(result));
			goto destroy_core_objs;
		}

		while ((result = doca_comm_channel_peer_addr_update_info(*peer_addr)) == DOCA_ERROR_CONNECTION_INPROGRESS)
			nanosleep(&ts, &ts);

		if (result != DOCA_SUCCESS) {
			DOCA_DLOG_ERR("Failed to validate the connection with the DPU: %s", doca_get_error_string(result));
			goto destroy_core_objs;
		}

		DOCA_LOG_INFO("Connection to DPU was established successfully");
	} else {
		// ==== server code =====
		result = doca_comm_channel_ep_listen(*ep, SERVER_NAME);
		if (result != DOCA_SUCCESS) {
			doca_dev_rep_close(cc_doca_dev_rep);
			DOCA_DLOG_ERR("Comm channel server couldn't start listening: %s", doca_get_error_string(result));
			goto destroy_core_objs;
		}

		DOCA_LOG_INFO("Started Listening, waiting for new connection");
	}

	return DOCA_SUCCESS;

destroy_core_objs:
	if (app_cfg->compress_method == COMPRESS_DEFLATE_HW)
		destroy_core_objects(state);
rep_dev_close:
	if (app_cfg->mode == SERVER)
		doca_dev_rep_close(cc_doca_dev_rep);
dev_close:
	doca_dev_close(cc_doca_dev);
compress_destroy:
	doca_compress_destroy(*compress_ctx);
	doca_comm_channel_ep_destroy(*ep);
	return result;
}

void
file_compression_cleanup(struct program_core_objects *state, struct file_compression_config *app_cfg,
			 struct doca_compress *compress_ctx, struct doca_comm_channel_ep_t *ep,
			 struct doca_comm_channel_addr_t **peer_addr)
{
	doca_error_t result;

	result = doca_comm_channel_ep_disconnect(ep, *peer_addr);
	if (result != DOCA_SUCCESS)
		DOCA_DLOG_ERR("Failed to disconnect channel: %s", doca_get_error_string(result));

	result = doca_comm_channel_ep_destroy(ep);
	if (result != DOCA_SUCCESS)
		DOCA_DLOG_ERR("Failed to destroy channel: %s", doca_get_error_string(result));

	if (app_cfg->compress_method == COMPRESS_DEFLATE_HW) {
		result = destroy_core_objects(state);
		if (result != DOCA_SUCCESS)
			DOCA_DLOG_ERR("Failed to destroy core objects: %s", doca_get_error_string(result));
	}

	result = doca_compress_destroy(compress_ctx);
	if (result != DOCA_SUCCESS)
		DOCA_DLOG_ERR("Failed to destroy compress: %s", doca_get_error_string(result));
}

/*
 * ARGP Callback - Handle file parameter
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t
file_callback(void *param, void *config)
{
	struct file_compression_config *app_cfg = (struct file_compression_config *)config;
	char *file_path = (char *)param;

	if (strnlen(file_path, MAX_FILE_NAME) == MAX_FILE_NAME) {
		DOCA_DLOG_ERR("File name is too long - MAX=%d", MAX_FILE_NAME - 1);
		return DOCA_ERROR_INVALID_VALUE;
	}
	strlcpy(app_cfg->file_path, file_path, MAX_FILE_NAME);
	return DOCA_SUCCESS;
}

/*
 * ARGP Callback - Handle file parameter
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t
out_file_callback(void *param, void *config)
{
	struct file_compression_config *app_cfg = (struct file_compression_config *)config;
	char *file_path = (char *)param;

	if (strnlen(file_path, MAX_FILE_NAME) == MAX_FILE_NAME) {
		DOCA_DLOG_ERR("File name is too long - MAX=%d", MAX_FILE_NAME - 1);
		return DOCA_ERROR_INVALID_VALUE;
	}
	strlcpy(app_cfg->out_file_path, file_path, MAX_FILE_NAME);
	return DOCA_SUCCESS;
}

/*
 * ARGP Callback - Handle Comm Channel DOCA device PCI address parameter
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t
dev_pci_addr_callback(void *param, void *config)
{
	struct file_compression_config *app_cfg = (struct file_compression_config *)config;
	char *pci_addr = (char *)param;

	if (strnlen(pci_addr, PCI_ADDR_LEN) == PCI_ADDR_LEN) {
		DOCA_DLOG_ERR("Entered device PCI address exceeding the maximum size of %d", PCI_ADDR_LEN - 1);
		return DOCA_ERROR_INVALID_VALUE;
	}
	strlcpy(app_cfg->cc_dev_pci_addr, pci_addr, PCI_ADDR_LEN);
	return DOCA_SUCCESS;
}

/*
 * ARGP Callback - Handle Comm Channel DOCA device representor PCI address parameter
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t
rep_pci_addr_callback(void *param, void *config)
{
	struct file_compression_config *app_cfg = (struct file_compression_config *)config;
	const char *rep_pci_addr = (char *)param;

	if (app_cfg->mode == SERVER) {
		if (strnlen(rep_pci_addr, PCI_ADDR_LEN) == PCI_ADDR_LEN) {
			DOCA_DLOG_ERR("Entered device representor PCI address exceeding the maximum size of %d",
				     PCI_ADDR_LEN - 1);
			return DOCA_ERROR_INVALID_VALUE;
		}

		strlcpy(app_cfg->cc_dev_rep_pci_addr, rep_pci_addr, PCI_ADDR_LEN);
	}

	return DOCA_SUCCESS;
}

/*
 * ARGP Callback - Handle timeout parameter
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t
timeout_callback(void *param, void *config)
{
	struct file_compression_config *app_cfg = (struct file_compression_config *)config;
	int *timeout = (int *)param;

	if (*timeout <= 0) {
		DOCA_DLOG_ERR("Timeout parameter must be positive value");
		return DOCA_ERROR_INVALID_VALUE;
	}
	app_cfg->timeout = *timeout;
	return DOCA_SUCCESS;
}

/*
 * ARGP validation Callback - check if the running mode is valid and that the input file exists in client mode
 *
 * @cfg [in]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t
args_validation_callback(void *cfg)
{
	struct file_compression_config *app_cfg = (struct file_compression_config *)cfg;

	if (app_cfg->mode == CLIENT && (access(app_cfg->file_path, F_OK) == -1)) {
		DOCA_DLOG_ERR("File was not found %s", app_cfg->file_path);
		return DOCA_ERROR_NOT_FOUND;
	} else if (app_cfg->mode == SERVER && strlen(app_cfg->cc_dev_rep_pci_addr) == 0) {
		DOCA_DLOG_ERR("Missing representor PCI address for server");
		return DOCA_ERROR_NOT_FOUND;
	}
	return DOCA_SUCCESS;
}

doca_error_t
register_file_compression_params()
{
	doca_error_t result;

	struct doca_argp_param *dev_pci_addr_param, *rep_pci_addr_param, *file_param, *out_file_param, *timeout_param;

	/* Create and register pci param */
	result = doca_argp_param_create(&dev_pci_addr_param);
	CHECK_ERR("Failed to create ARGP param: %s", doca_get_error_string(result))
	doca_argp_param_set_short_name(dev_pci_addr_param, "p");
	doca_argp_param_set_long_name(dev_pci_addr_param, "pci-addr");
	doca_argp_param_set_description(dev_pci_addr_param, "DOCA Comm Channel device PCI address");
	doca_argp_param_set_callback(dev_pci_addr_param, dev_pci_addr_callback);
	doca_argp_param_set_type(dev_pci_addr_param, DOCA_ARGP_TYPE_STRING);
	doca_argp_param_set_mandatory(dev_pci_addr_param);
	result = doca_argp_register_param(dev_pci_addr_param);
	CHECK_ERR("Failed to register program param: %s", doca_get_error_string(result))

	/* Create and register rep PCI address param */
	result = doca_argp_param_create(&rep_pci_addr_param);
	CHECK_ERR("Failed to create ARGP param: %s", doca_get_error_string(result))
	doca_argp_param_set_short_name(rep_pci_addr_param, "r");
	doca_argp_param_set_long_name(rep_pci_addr_param, "rep-pci");
	doca_argp_param_set_description(rep_pci_addr_param, "DOCA Comm Channel device representor PCI address");
	doca_argp_param_set_callback(rep_pci_addr_param, rep_pci_addr_callback);
	doca_argp_param_set_type(rep_pci_addr_param, DOCA_ARGP_TYPE_STRING);
	result = doca_argp_register_param(rep_pci_addr_param);
	CHECK_ERR("Failed to register program param: %s", doca_get_error_string(result))

	/* Create and register message to send param */
	result = doca_argp_param_create(&file_param);
	CHECK_ERR("Failed to create ARGP param: %s", doca_get_error_string(result))
	doca_argp_param_set_short_name(file_param, "f");
	doca_argp_param_set_long_name(file_param, "file");
	doca_argp_param_set_description(file_param, "File to send by the client");
	doca_argp_param_set_callback(file_param, file_callback);
	doca_argp_param_set_type(file_param, DOCA_ARGP_TYPE_STRING);
	result = doca_argp_register_param(file_param);
	CHECK_ERR("Failed to register program param: %s", doca_get_error_string(result))

	/* Create and register output file param */
	result = doca_argp_param_create(&out_file_param);
	CHECK_ERR("Failed to create ARGP param: %s", doca_get_error_string(result))
	doca_argp_param_set_short_name(out_file_param, "o");
	doca_argp_param_set_long_name(out_file_param, "out");
	doca_argp_param_set_description(out_file_param, "File to write the compressed data(output)");
	doca_argp_param_set_callback(out_file_param, out_file_callback);
	doca_argp_param_set_type(out_file_param, DOCA_ARGP_TYPE_STRING);
	result = doca_argp_register_param(out_file_param);
	CHECK_ERR("Failed to register program param: %s", doca_get_error_string(result))

	/* Create and register timeout */
	result = doca_argp_param_create(&timeout_param);
	CHECK_ERR("Failed to create ARGP param: %s", doca_get_error_string(result))
	doca_argp_param_set_short_name(timeout_param, "t");
	doca_argp_param_set_long_name(timeout_param, "timeout");
	doca_argp_param_set_description(timeout_param, "Application timeout for receiving file content messages, default is 5 sec");
	doca_argp_param_set_callback(timeout_param, timeout_callback);
	doca_argp_param_set_type(timeout_param, DOCA_ARGP_TYPE_INT);
	result = doca_argp_register_param(timeout_param);
	CHECK_ERR("Failed to register program param: %s", doca_get_error_string(result))

	/* Register version callback for DOCA SDK & RUNTIME */
	result = doca_argp_register_version_callback(sdk_version_callback);
	CHECK_ERR("Failed to register version callback: %s", doca_get_error_string(result))

	/* Register application callback */
	result = doca_argp_register_validation_callback(args_validation_callback);
	CHECK_ERR("Failed to register program validation callback: %s", doca_get_error_string(result))

	return result;
}
