/*--------------------------------------------------------------- 
 * Copyright (c) 2010                              
 * BNL            
 * All Rights Reserved.                                           
 *--------------------------------------------------------------- 
 * Permission is hereby granted, free of charge, to any person    
 * obtaining a copy of this software (Iperf) and associated       
 * documentation files (the "Software"), to deal in the Software  
 * without restriction, including without limitation the          
 * rights to use, copy, modify, merge, publish, distribute,        
 * sublicense, and/or sell copies of the Software, and to permit     
 * persons to whom the Software is furnished to do
 * so, subject to the following conditions: 
 *
 *     
 * Redistributions of source code must retain the above 
 * copyright notice, this list of conditions and 
 * the following disclaimers. 
 *
 *     
 * Redistributions in binary form must reproduce the above 
 * copyright notice, this list of conditions and the following 
 * disclaimers in the documentation and/or other materials 
 * provided with the distribution. 
 * 
 *     
 * Neither the names of the University of Illinois, NCSA, 
 * nor the names of its contributors may be used to endorse 
 * or promote products derived from this Software without
 * specific prior written permission. 
 * 
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, 
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES 
 * OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND 
 * NONINFRINGEMENT. IN NO EVENT SHALL THE CONTIBUTORS OR COPYRIGHT 
 * HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, 
 * WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, 
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE. 
 * ________________________________________________________________
 * National Laboratory for Applied Network Research 
 * National Center for Supercomputing Applications 
 * University of Illinois at Urbana-Champaign 
 * http://www.ncsa.uiuc.edu
 * ________________________________________________________________ 
 *
 * rdma.c
 * by Yufei Ren <renyufei83@gmail.com>
 * -------------------------------------------------------------------
 * An abstract class for waiting on a condition variable. If
 * threads are not available, this does nothing.
 * ------------------------------------------------------------------- */

#include "headers.h"
#include "rdma.h"

extern struct acptq acceptedTqh;

static int server_recv(struct rdma_cb *cb, struct ibv_wc *wc)
{
	if (wc->byte_len != sizeof(cb->recv_buf)) {
		fprintf(stderr, "Received bogus data, size %d\n", wc->byte_len);
		return -1;
	}

	cb->remote_rkey = ntohl(cb->recv_buf.rkey);
	cb->remote_addr = ntohll(cb->recv_buf.buf);
	cb->remote_len  = ntohl(cb->recv_buf.size);
	cb->remote_mode = ntohl(cb->recv_buf.mode);
	DEBUG_LOG("Received rkey %x addr %" PRIx64 " len %d from peer\n",
		  cb->remote_rkey, cb->remote_addr, cb->remote_len);
	
	switch ( cb->remote_mode ) {
	case MODE_RDMA_ACTRD:
		cb->trans_mode = kRdmaTrans_PasRead;
		break;
	case MODE_RDMA_ACTWR:
		cb->trans_mode = kRdmaTrans_PasWrte;
		break;
	case MODE_RDMA_PASRD:
		cb->trans_mode = kRdmaTrans_ActRead;
		break;
	case MODE_RDMA_PASWR:
		cb->trans_mode = kRdmaTrans_ActWrte;
		break;
	default:
		fprintf(stderr, "unrecognize transfer mode %d\n", \
			cb->remote_mode);
		break;
	}
	
	if (cb->state <= CONNECTED || cb->state == RDMA_WRITE_COMPLETE)
		cb->state = RDMA_READ_ADV;
	else
		cb->state = RDMA_WRITE_ADV;
	DPRINTF(("server_recv success\n"));
	
	return 0;
}

static int client_recv(struct rdma_cb *cb, struct ibv_wc *wc)
{
	if (wc->byte_len != sizeof(cb->recv_buf)) {
		fprintf(stderr, "Received bogus data, size %d\n", wc->byte_len);
		return -1;
	}

	if (cb->state == RDMA_READ_ADV)
		cb->state = RDMA_WRITE_ADV;
	else
		cb->state = RDMA_WRITE_COMPLETE;

	return 0;
}


int iperf_cma_event_handler(struct rdma_cm_id *cma_id,
				    struct rdma_cm_event *event)
{
	int ret = 0;
	struct rdma_cb *cb = cma_id->context;

	DEBUG_LOG("cma_event type %s cma_id %p (%s)\n",
		  rdma_event_str(event->event), cma_id,
		  (cma_id == cb->cm_id) ? "parent" : "child");
		  
	DPRINTF(("1 cq_handler cb @ %x\n", (unsigned long)cb));

	switch (event->event) {
	case RDMA_CM_EVENT_ADDR_RESOLVED:
		cb->state = ADDR_RESOLVED;
		ret = rdma_resolve_route(cma_id, 2000);
		if (ret) {
			cb->state = ERROR;
			perror("rdma_resolve_route");
			sem_post(&cb->sem);
		}
		break;

	case RDMA_CM_EVENT_ROUTE_RESOLVED:
		cb->state = ROUTE_RESOLVED;
		sem_post(&cb->sem);
		break;

	case RDMA_CM_EVENT_CONNECT_REQUEST:
		cb->state = CONNECT_REQUEST;
		
		TAILQ_LOCK(&acceptedTqh);
		struct wcm_id *item = \
			(struct wcm_id *) malloc(sizeof(struct wcm_id));
		if (item == NULL) {
			fprintf(stderr, "Out of Memory\n");
			exit(EXIT_FAILURE);
		}
		
		item->child_cm_id = cma_id;
				
		TAILQ_INSERT_TAIL(&acceptedTqh, item, entries);
		
		TAILQ_UNLOCK(&acceptedTqh);
		TAILQ_SIGNAL(&acceptedTqh);
		cb->child_cm_id = cma_id;
		DEBUG_LOG("child cma %p\n", cb->child_cm_id);
//		sem_post(&cb->sem);
		break;

	case RDMA_CM_EVENT_ESTABLISHED:
		DEBUG_LOG("ESTABLISHED\n");

		/*
		 * Server will wake up when first RECV completes.
		 */
		if (!cb->server) {
			cb->state = CONNECTED;
		}
		sem_post(&cb->sem);
		break;

	case RDMA_CM_EVENT_ADDR_ERROR:
	case RDMA_CM_EVENT_ROUTE_ERROR:
	case RDMA_CM_EVENT_CONNECT_ERROR:
	case RDMA_CM_EVENT_UNREACHABLE:
	case RDMA_CM_EVENT_REJECTED:
		fprintf(stderr, "cma event %s, error %d\n",
			rdma_event_str(event->event), event->status);
		sem_post(&cb->sem);
		ret = -1;
		break;

	case RDMA_CM_EVENT_DISCONNECTED:
		fprintf(stderr, "RDMA %s DISCONNECT EVENT...\n",
			cb->server ? "server" : "client");
		sem_post(&cb->sem);
		break;

	case RDMA_CM_EVENT_DEVICE_REMOVAL:
		fprintf(stderr, "cma detected device removal!!!!\n");
		ret = -1;
		break;

	default:
		fprintf(stderr, "unhandled event: %s, ignoring\n",
			rdma_event_str(event->event));
		break;
	}

	return ret;
}

void *cm_thread(void *arg) {
	struct rdma_cb *cb = arg;
	struct rdma_cm_event *event;
	int ret;

	while (1) {
		ret = rdma_get_cm_event(cb->cm_channel, &event);
		if (ret) {
			perror("rdma_get_cm_event");
			exit(ret);
		}
		ret = iperf_cma_event_handler(event->id, event);
		rdma_ack_cm_event(event);
		if (ret)
			exit(ret);
	}
}


int iperf_cq_event_handler(struct rdma_cb *cb)
{
	struct ibv_wc wc;
	struct ibv_recv_wr *bad_wr;
	int ret;
	
	DPRINTF(("2 cq_handler cb @ %x\n", (unsigned long)cb));
	DPRINTF(("cq_handler sem_post @ %x\n", (unsigned long)&cb->sem));

	while ((ret = ibv_poll_cq(cb->cq, 1, &wc)) == 1) {
		ret = 0;

		if (wc.status) {
			fprintf(stderr, "cq completion failed status %d\n",
				wc.status);
			// IBV_WC_WR_FLUSH_ERR == 5
			if (wc.status != IBV_WC_WR_FLUSH_ERR)
				ret = -1;
			goto error;
		}

		switch (wc.opcode) {
		case IBV_WC_SEND:
			DEBUG_LOG("send completion\n");
			break;

		case IBV_WC_RDMA_WRITE:
			DEBUG_LOG("rdma write completion\n");
			cb->state = RDMA_WRITE_COMPLETE;
			sem_post(&cb->sem);
			break;

		case IBV_WC_RDMA_READ:
			DEBUG_LOG("rdma read completion\n");
			cb->state = RDMA_READ_COMPLETE;
			sem_post(&cb->sem);
			break;

		case IBV_WC_RECV:
			DEBUG_LOG("recv completion\n");
			DPRINTF(("IBV_WC_RECV cb->server %d\n", cb->server));
			ret = cb->server ? server_recv(cb, &wc) :
					   client_recv(cb, &wc);
			if (ret) {
				fprintf(stderr, "recv wc error: %d\n", ret);
				goto error;
			}

			ret = ibv_post_recv(cb->qp, &cb->rq_wr, &bad_wr);
			if (ret) {
				fprintf(stderr, "post recv error: %d\n", ret);
				goto error;
			}
			sem_post(&cb->sem);
			DPRINTF(("IBV_WC_RECV success\n"));
			break;

		default:
			DEBUG_LOG("unknown!!!!! completion\n");
			ret = -1;
			goto error;
		}
	}
	if (ret) {
		fprintf(stderr, "poll error %d\n", ret);
		goto error;
	}
	return 0;

error:
	cb->state = ERROR;
	sem_post(&cb->sem);
	return ret;
}


void *cq_thread(void *arg)
{
	struct rdma_cb *cb = arg;
	struct ibv_cq *ev_cq;
	void *ev_ctx;
	int ret;
	
	DEBUG_LOG("cq_thread started.\n");

	while (1) {	
		pthread_testcancel();

		ret = ibv_get_cq_event(cb->channel, &ev_cq, &ev_ctx);
		if (ret) {
			fprintf(stderr, "Failed to get cq event!\n");
			pthread_exit(NULL);
		}
		if (ev_cq != cb->cq) {
			fprintf(stderr, "Unknown CQ!\n");
			pthread_exit(NULL);
		}
		ret = ibv_req_notify_cq(cb->cq, 0);
		if (ret) {
			fprintf(stderr, "Failed to set notify!\n");
			pthread_exit(NULL);
		}
		ret = iperf_cq_event_handler(cb);
		ibv_ack_cq_events(cb->cq, 1);
		if (ret)
			pthread_exit(NULL);
	}
}


int rdma_init( struct rdma_cb *cb ) {
	int ret = 0;
	
//	sem_init(&cb->sem, 0, 0);

/*	cb = malloc(sizeof(*cb));
	if (!cb)
		return -ENOMEM;

	// rdma_thr->cb = cb;
*/
	cb->cm_channel = rdma_create_event_channel();
	if (!cb->cm_channel) {
		perror("rdma_create_event_channel");
		free(cb);
		return -1;
	}

	ret = rdma_create_id(cb->cm_channel, &cb->cm_id, cb, RDMA_PS_TCP);
	if (ret) {
		perror("rdma_create_id");
		rdma_destroy_event_channel(cb->cm_channel);
		free(cb);
		return -1;
	}

	pthread_create(&cb->cmthread, NULL, cm_thread, cb);

/*	if (cb->server) {
		if (persistent_server)
			ret = rping_run_persistent_server(cb);
		else
			ret = rping_run_server(cb);
	} else
		ret = rping_run_client(cb);
*/
	// rdma_destroy_id(cb->cm_id);
	
	return 0;
}


// setup queue pair
int iperf_setup_qp(struct rdma_cb *cb, struct rdma_cm_id *cm_id)
{
	int ret;

	cb->pd = ibv_alloc_pd(cm_id->verbs);
	if (!cb->pd) {
		fprintf(stderr, "ibv_alloc_pd failed\n");
		return errno;
	}
	DEBUG_LOG("created pd %p\n", cb->pd);

	cb->channel = ibv_create_comp_channel(cm_id->verbs);
	if (!cb->channel) {
		fprintf(stderr, "ibv_create_comp_channel failed\n");
		ret = errno;
		goto err1;
	}
	DEBUG_LOG("created channel %p\n", cb->channel);

	cb->cq = ibv_create_cq(cm_id->verbs, IPERF_RDMA_SQ_DEPTH * 2, cb,
				cb->channel, 0);
	if (!cb->cq) {
		fprintf(stderr, "ibv_create_cq failed\n");
		ret = errno;
		goto err2;
	}
	DEBUG_LOG("created cq %p\n", cb->cq);
	
	DEBUG_LOG("before ibv_req_notify_cq\n");
	ret = ibv_req_notify_cq(cb->cq, 0);
	if (ret) {
		fprintf(stderr, "ibv_create_cq failed\n");
		ret = errno;
		goto err3;
	}

	DPRINTF(("before iperf_create_qp\n"));
	ret = iperf_create_qp(cb);
	if (ret) {
		perror("rdma_create_qp");
		goto err3;
	}
	DEBUG_LOG("created qp %p\n", cb->qp);
	return 0;

err3:
	ibv_destroy_cq(cb->cq);
err2:
	ibv_destroy_comp_channel(cb->channel);
err1:
	ibv_dealloc_pd(cb->pd);
	return ret;
}


int iperf_create_qp(struct rdma_cb *cb)
{
	struct ibv_qp_init_attr init_attr;
	int ret;

	memset(&init_attr, 0, sizeof(init_attr));
	init_attr.cap.max_send_wr = IPERF_RDMA_SQ_DEPTH;
	init_attr.cap.max_recv_wr = 2;
	init_attr.cap.max_recv_sge = 1;
	init_attr.cap.max_send_sge = 1;
	init_attr.qp_type = IBV_QPT_RC;
	init_attr.send_cq = cb->cq;
	init_attr.recv_cq = cb->cq;

	DPRINTF(("before rdma_create_qp\n"));
	DPRINTF(("cb->server: %d\n", cb->server));
	if (cb->server) {
		ret = rdma_create_qp(cb->child_cm_id, cb->pd, &init_attr);
		if (!ret)
			cb->qp = cb->child_cm_id->qp;
	} else {
		ret = rdma_create_qp(cb->cm_id, cb->pd, &init_attr);
		if (!ret)
			cb->qp = cb->cm_id->qp;
	}
	DPRINTF(("after rdma_create_qp, ret = %d\n", ret));

	return ret;
}

void iperf_free_qp(struct rdma_cb *cb)
{
	ibv_destroy_qp(cb->qp);
	ibv_destroy_cq(cb->cq);
	ibv_destroy_comp_channel(cb->channel);
	ibv_dealloc_pd(cb->pd);
}


int iperf_setup_buffers(struct rdma_cb *cb)
{
	int ret;

	DEBUG_LOG("rping_setup_buffers called on cb %p\n", cb);

	cb->recv_mr = ibv_reg_mr(cb->pd, &cb->recv_buf, sizeof cb->recv_buf,
				 IBV_ACCESS_LOCAL_WRITE);
	if (!cb->recv_mr) {
		fprintf(stderr, "recv_buf reg_mr failed\n");
		return errno;
	}

	cb->send_mr = ibv_reg_mr(cb->pd, &cb->send_buf, sizeof cb->send_buf, 0);
	if (!cb->send_mr) {
		fprintf(stderr, "send_buf reg_mr failed\n");
		ret = errno;
		goto err1;
	}

	cb->rdma_buf = malloc(cb->size);
	if (!cb->rdma_buf) {
		fprintf(stderr, "rdma_buf malloc failed\n");
		ret = -ENOMEM;
		goto err2;
	}

	cb->rdma_mr = ibv_reg_mr(cb->pd, cb->rdma_buf, cb->size,
				 IBV_ACCESS_LOCAL_WRITE |
				 IBV_ACCESS_REMOTE_READ |
				 IBV_ACCESS_REMOTE_WRITE);
	if (!cb->rdma_mr) {
		fprintf(stderr, "rdma_buf reg_mr failed\n");
		ret = errno;
		goto err3;
	}

	if (!cb->server) {
		cb->start_buf = malloc(cb->size);
		if (!cb->start_buf) {
			fprintf(stderr, "start_buf malloc failed\n");
			ret = -ENOMEM;
			goto err4;
		}

		cb->start_mr = ibv_reg_mr(cb->pd, cb->start_buf, cb->size,
					  IBV_ACCESS_LOCAL_WRITE | 
					  IBV_ACCESS_REMOTE_READ |
					  IBV_ACCESS_REMOTE_WRITE);
		if (!cb->start_mr) {
			fprintf(stderr, "start_buf reg_mr failed\n");
			ret = errno;
			goto err5;
		}
	}

	iperf_setup_wr(cb);
	DEBUG_LOG("allocated & registered buffers...\n");
	return 0;

err5:
	free(cb->start_buf);
err4:
	ibv_dereg_mr(cb->rdma_mr);
err3:
	free(cb->rdma_buf);
err2:
	ibv_dereg_mr(cb->send_mr);
err1:
	ibv_dereg_mr(cb->recv_mr);
	return ret;
}


void iperf_free_buffers(struct rdma_cb *cb)
{
	DEBUG_LOG("rping_free_buffers called on cb %p\n", cb);
	ibv_dereg_mr(cb->recv_mr);
	ibv_dereg_mr(cb->send_mr);
	ibv_dereg_mr(cb->rdma_mr);
	free(cb->rdma_buf);
	if (!cb->server) {
		ibv_dereg_mr(cb->start_mr);
		free(cb->start_buf);
	}
}


void iperf_setup_wr(struct rdma_cb *cb)
{
	cb->recv_sgl.addr = (uint64_t) (unsigned long) &cb->recv_buf;
	cb->recv_sgl.length = sizeof cb->recv_buf;
	cb->recv_sgl.lkey = cb->recv_mr->lkey;
	cb->rq_wr.sg_list = &cb->recv_sgl;
	cb->rq_wr.num_sge = 1;

	cb->send_sgl.addr = (uint64_t) (unsigned long) &cb->send_buf;
	cb->send_sgl.length = sizeof cb->send_buf;
	cb->send_sgl.lkey = cb->send_mr->lkey;

	cb->sq_wr.opcode = IBV_WR_SEND;
	cb->sq_wr.send_flags = IBV_SEND_SIGNALED;
	cb->sq_wr.sg_list = &cb->send_sgl;
	cb->sq_wr.num_sge = 1;

	cb->rdma_sgl.addr = (uint64_t) (unsigned long) cb->rdma_buf;
	cb->rdma_sgl.lkey = cb->rdma_mr->lkey;
	cb->rdma_sq_wr.send_flags = IBV_SEND_SIGNALED;
	cb->rdma_sq_wr.sg_list = &cb->rdma_sgl;
	cb->rdma_sq_wr.num_sge = 1;
}


int rdma_connect_client(struct rdma_cb *cb)
{
	struct rdma_conn_param conn_param;
	int ret;

	memset(&conn_param, 0, sizeof conn_param);
	conn_param.responder_resources = 1;
	conn_param.initiator_depth = 1;
	conn_param.retry_count = 10;

	ret = rdma_connect(cb->cm_id, &conn_param);
	if (ret) {
		perror("rdma_connect");
		return ret;
	}

	sem_wait(&cb->sem);
	if (cb->state != CONNECTED) {
		fprintf(stderr, "wait for CONNECTED state %d\n", cb->state);
		return -1;
	}

	DEBUG_LOG("rdma_connect successful\n");
	return 0;
}


int iperf_accept(struct rdma_cb *cb)
{
	struct rdma_conn_param conn_param;
	int ret;

	DEBUG_LOG("accepting client connection request\n");

	memset(&conn_param, 0, sizeof conn_param);
	conn_param.responder_resources = 1;
	conn_param.initiator_depth = 1;

	DPRINTF(("tid %ld, child_cm_id %p\n", pthread_self(), cb->child_cm_id));

	ret = rdma_accept(cb->child_cm_id, &conn_param);
	if (ret) {
		perror("rdma_accept");
		return ret;
	}

	sem_wait(&cb->sem);
	if (cb->state == ERROR) {
		fprintf(stderr, "wait for CONNECTED state %d\n", cb->state);
		return -1;
	}
	DPRINTF(("iperf_accept finish with state %d\n", cb->state));
	return 0;
}


void iperf_format_send(struct rdma_cb *cb, char *buf, struct ibv_mr *mr)
{
	struct iperf_rdma_info *info = &cb->send_buf;

	info->buf = htonll((uint64_t) (unsigned long) buf);
	info->rkey = htonl(mr->rkey);
	info->size = htonl(cb->size);
	
	switch ( cb->trans_mode ) {
	case kRdmaTrans_ActRead:
		info->mode = htonl(MODE_RDMA_ACTRD);
		break;
	case kRdmaTrans_ActWrte:
		info->mode = htonl(MODE_RDMA_ACTWR);
		break;
	case kRdmaTrans_PasRead:
		info->mode = htonl(MODE_RDMA_PASRD);
		break;
	case kRdmaTrans_PasWrte:
		info->mode = htonl(MODE_RDMA_PASWR);
		break;
	default:
		fprintf(stderr, "unrecognize transfer mode %d\n", \
			cb->trans_mode);
		break;
	}

	DEBUG_LOG("RDMA addr %" PRIx64" rkey %x len %d\n",
		  ntohll(info->buf), ntohl(info->rkey), ntohl(info->size));
}


int cli_act_rdma_rd(struct rdma_cb *cb)
{
	return 0;
}

int cli_act_rdma_wr(struct rdma_cb *cb)
{
	return 0;
}

int cli_pas_rdma_rd(struct rdma_cb *cb)
{
	int err;
	struct ibv_send_wr* bad_wr;
	
	cb->state = RDMA_READ_ADV;
	
	iperf_format_send(cb, cb->start_buf, cb->start_mr);

	err = ibv_post_send(cb->qp, &cb->sq_wr, &bad_wr);
	if (err) {
		fprintf(stderr, "post send error %d\n", err);
		return -err;
	}
	DPRINTF(("client ibv_post_send success\n"));

	/* Wait for server to ACK read complete */
	DPRINTF(("client RunRDMA cb @ %x\n", (unsigned long)cb));
	DPRINTF(("client RunRDMA sem_wait @ %x\n", (unsigned long)&cb->sem));
	DPRINTF(("wait server to say go ahead\n"));
	sem_wait(&cb->sem);
	if (cb->state != RDMA_WRITE_ADV) {
		fprintf(stderr, "wait for RDMA_WRITE_ADV state %d\n",
			cb->state);
		err = -1;
		return err;
	}
	DPRINTF(("client: server ACK read complete success\n"));
	
	return cb->size;
}

int cli_pas_rdma_wr(struct rdma_cb *cb)
{
	int err;
	struct ibv_send_wr* bad_wr;
	
	cb->state = RDMA_WRITE_ADV;

	iperf_format_send(cb, cb->rdma_buf, cb->rdma_mr);
	err = ibv_post_send(cb->qp, &cb->sq_wr, &bad_wr);
	if (err) {
		fprintf(stderr, "post send error %d\n", err);
		return -err;
	}
	DPRINTF(("cli_pas_rdma_wr: ibv_post_send success\n"));

	/* Wait for the server to say the RDMA Write is complete. */
	sem_wait(&cb->sem);
	if (cb->state != RDMA_WRITE_COMPLETE) {
		fprintf(stderr, "wait for RDMA_WRITE_COMPLETE state %d\n",
			cb->state);
		err = -1;
		return err;
	}
	DPRINTF(("cli_pas_rdma_wr: RDMA_WRITE_COMPLETE\n"));

	return cb->size;
}

int svr_act_rdma_rd(struct rdma_cb *cb)
{
	int ret;
	
	struct ibv_send_wr *bad_send_wr;
	struct ibv_recv_wr *bad_recv_wr;
	
	/* Issue RDMA Read. */
	cb->rdma_sq_wr.opcode = IBV_WR_RDMA_READ;
	cb->rdma_sq_wr.wr.rdma.rkey = cb->remote_rkey;
	cb->rdma_sq_wr.wr.rdma.remote_addr = cb->remote_addr;
	cb->rdma_sq_wr.sg_list->length = cb->remote_len;
	
	ret = ibv_post_send(cb->qp, &cb->rdma_sq_wr, &bad_send_wr);
	if (ret) {
		fprintf(stderr, "post send error %d\n", ret);
		return ret;
	}
	DEBUG_LOG("server posted rdma read req\n");
//if (cb->firstrans == 0)
//printf("@ %x server posted rdma read req\n", (unsigned long)&cb->sem);
	/* Wait for read completion */
	sem_wait(&cb->sem);
	if (cb->state != RDMA_READ_COMPLETE) {
		fprintf(stderr, "wait for RDMA_READ_COMPLETE state %d\n",
			cb->state);
		ret = -1;
		return ret;
	}
	DEBUG_LOG("server received read complete\n");
//if (cb->firstrans == 0)
//printf("@ %x server received read complete\n", (unsigned long)&cb->sem);
	/* Display data in recv buf */
	if (cb->verbose)
		printf("server ping data: %s\n", cb->rdma_buf);
	
	/* write data to file output */
	if (cb->outputfile != NULL)
	    if ( fwrite( cb->rdma_buf, cb->remote_len, 1, cb->outputfile ) < 0 )
	        fprintf( stderr, "Unable to write to the file stream\n");

	/* Tell client to continue */
	ret = ibv_post_send(cb->qp, &cb->sq_wr, &bad_send_wr);
	if (ret) {
		fprintf(stderr, "post send error %d\n", ret);
		return ret;
	}
	DEBUG_LOG("server posted go ahead\n");
//if (cb->firstrans == 0)
//printf("@ %x server posted go ahead\n", (unsigned long)&cb->sem);
//cb->firstrans = 1;
	/* Wait for client's RDMA STAG/TO/Len
	sem_wait(&mCb->sem);
	if (mCb->state != RDMA_WRITE_ADV) {
		fprintf(stderr, "wait for RDMA_WRITE_ADV state %d\n",
			mCb->state);
		ret = -1;
		break;
	}
	DEBUG_LOG("server received sink adv\n"); */

	/* RDMA Write echo data
	mCb->rdma_sq_wr.opcode = IBV_WR_RDMA_WRITE;
	mCb->rdma_sq_wr.wr.rdma.rkey = mCb->remote_rkey;
	mCb->rdma_sq_wr.wr.rdma.remote_addr = mCb->remote_addr;
	mCb->rdma_sq_wr.sg_list->length = strlen(mCb->rdma_buf) + 1;
	DEBUG_LOG("rdma write from lkey %x laddr %x len %d\n",
		  mCb->rdma_sq_wr.sg_list->lkey,
		  mCb->rdma_sq_wr.sg_list->addr,
		  mCb->rdma_sq_wr.sg_list->length);

	ret = ibv_post_send(mCb->qp, &mCb->rdma_sq_wr, &bad_send_wr);
	if (ret) {
		fprintf(stderr, "post send error %d\n", ret);
		break;
	} */

	/* Wait for completion
	ret = sem_wait(&mCb->sem);
	if (mCb->state != RDMA_WRITE_COMPLETE) {
		fprintf(stderr, "wait for RDMA_WRITE_COMPLETE state %d\n",
			mCb->state);
		ret = -1;
		break;
	}
	DEBUG_LOG("server rdma write complete \n"); */

	/* Tell client to begin again
	ret = ibv_post_send(mCb->qp, &mCb->sq_wr, &bad_send_wr);
	if (ret) {
		fprintf(stderr, "post send error %d\n", ret);
		break;
	}
	DEBUG_LOG("server posted go ahead\n"); */

	return cb->remote_len;
}

int svr_act_rdma_wr(struct rdma_cb *cb)
{
	int ret;
	
	struct ibv_send_wr *bad_send_wr;
	struct ibv_recv_wr *bad_recv_wr;

	/* Wait for client's RDMA STAG/TO/Len */
	if ( cb->state == RDMA_READ_ADV ) // established, key recved
		cb->state = RDMA_WRITE_ADV;
	else {// established, key not recved
		cb->state = RDMA_READ_COMPLETE;
		sem_wait(&cb->sem);
	}
	DPRINTF(("sem_wait @ %x RDMA_WRITE_ADV\n", \
		(unsigned long)&cb->sem));
	if (cb->state != RDMA_WRITE_ADV) {
		fprintf(stderr, "wait for RDMA_WRITE_ADV state %d\n",
			cb->state);
		ret = -1;
		return ret;
	}
	DEBUG_LOG("server received sink adv\n");

	/* RDMA Write echo data */
	cb->rdma_sq_wr.opcode = IBV_WR_RDMA_WRITE;
	cb->rdma_sq_wr.wr.rdma.rkey = cb->remote_rkey;
	cb->rdma_sq_wr.wr.rdma.remote_addr = cb->remote_addr;
//	cb->rdma_sq_wr.sg_list->length = strlen(cb->rdma_buf) + 1;
	cb->rdma_sq_wr.sg_list->length = cb->remote_len;
/* ?????????????? */
	DEBUG_LOG("rdma write from lkey %x laddr %x len %d\n",
		  cb->rdma_sq_wr.sg_list->lkey,
		  cb->rdma_sq_wr.sg_list->addr,
		  cb->rdma_sq_wr.sg_list->length);
	
	ret = ibv_post_send(cb->qp, &cb->rdma_sq_wr, &bad_send_wr);
	if (ret) {
		fprintf(stderr, "post send error %d\n", ret);
		return -ret;
	}

	/* Wait for completion */
	ret = sem_wait(&cb->sem);
	if (cb->state != RDMA_WRITE_COMPLETE) {
		fprintf(stderr, "wait for RDMA_WRITE_COMPLETE state %d\n",
			cb->state);
		ret = -1;
		return ret;
	}
	DEBUG_LOG("server rdma write complete \n");

	/* Tell client to begin again */
	ret = ibv_post_send(cb->qp, &cb->sq_wr, &bad_send_wr);
	if (ret) {
		fprintf(stderr, "post send error %d\n", ret);
		return -ret;
	}
	DEBUG_LOG("server posted go ahead\n");

	return cb->remote_len;
}

int svr_pas_rdma_rd(struct rdma_cb *cb)
{
	return 0;
}

int svr_pas_rdma_wr(struct rdma_cb *cb)
{
	return 0;
}
