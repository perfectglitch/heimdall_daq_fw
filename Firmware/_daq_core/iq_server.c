/*
 * 
 * Description :
 * IQ frame Ethernet server
 * 
 *
 * Project : HeIMDALL DAQ Firmware
 * License : GNU GPL V3
 * Author  : Tamas Peto
 * 
 * Copyright (C) 2018-2020  Tamás Pető
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 *
 */
 
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <signal.h>

#include "eth_server.h"
#include "ini.h"
#include "log.h"
#include "sh_mem_util.h"
#include "iq_header.h"
#include "im_msg.h"
#define INI_FNAME "daq_chain_config.ini" 

#define FATAL_ERR(l) log_fatal(l); return -1;

/*
 * This structure stores the configuration parameters, 
 * that are loaded from the ini file
 */ 
typedef struct
{
    int num_ch;
    int cpi_size;    
    int log_level;      
} configuration;

/*
 * Ini configuration parser callback function  
*/
static int handler(void* conf_struct, const char* section, const char* name,
                   const char* value)

{
    configuration* pconfig = (configuration*) conf_struct;

    #define MATCH(s, n) strcmp(section, s) == 0 && strcmp(name, n) == 0
    if (MATCH("hw", "num_ch")) 
    {
        pconfig->num_ch = atoi(value);
    } 
    else if (MATCH("pre_processing", "cpi_size")) 
    {
        pconfig->cpi_size = atoi(value);
    }
    else if (MATCH("daq", "log_level")) 
    {
        pconfig->log_level = atoi(value);
    }
    else {
        return 0;  /* unknown section/name, error */
    }
    return 0;
}

/*
 * send() on a TCP socket is not guaranteed to transmit the whole buffer in
 * one call -- for large buffers (our payload can be tens of MB, well beyond
 * the kernel's socket send buffer) it will routinely return a short count.
 * This loops until either everything has been sent or a real error occurs,
 * so large frames don't get silently truncated (which desyncs every frame
 * boundary after it for the client).
 */
static int send_all(int socket, const void* buf, size_t len)
{
    const char* p = (const char*) buf;
    size_t sent = 0;
    while (sent < len)
    {
        ssize_t n = send(socket, p + sent, len - sent, MSG_NOSIGNAL);
        if (n < 0)
        {
            if (errno == EINTR) continue;
            return -1;
        }
        sent += (size_t) n;
    }
    return 0;
}

int send_iq_frame(struct iq_frame_struct_32* iq_frame, int socket)
{
    int transfer_size = iq_frame->payload_size*sizeof(*iq_frame->payload)*2+IQ_HEADER_LENGTH;

    // Sending header
    if (send_all(socket, iq_frame->header, sizeof(struct iq_header_struct)) != 0)
    {log_error("Ethernet transfer failed (header)"); return -1;}
    // Sending payload
    if (iq_frame->payload_size != 0)
    {
        if (send_all(socket, iq_frame->payload, transfer_size-IQ_HEADER_LENGTH) != 0)
        {log_error("Ethernet transfer failed (payload)"); return -1;}
    }
    return 0;
}

int main(int argc, char* argv[])
{
    // A client (e.g. a GNU Radio flowgraph) disconnecting mid-transfer is
    // routine, not exceptional -- without this, the next send() on the now-
    // closed socket raises SIGPIPE, whose default action silently kills this
    // whole process (cascading into a broken-pipe crash in delay_sync.py,
    // which writes to this process via a control FIFO). MSG_NOSIGNAL on the
    // send() call in send_all() covers the socket specifically; this is
    // defense in depth for any other write path.
    signal(SIGPIPE, SIG_IGN);

    log_set_level(LOG_TRACE);
    configuration config;
	int ret = 0;
    int active_buff_ind;
    char eth_cmd[1024]; // Ethernet command buffer   
	   
    /* Set parameters from the config file*/
    if (ini_parse(INI_FNAME, handler, &config) < 0)
    {FATAL_ERR("Configuration could not be loaded, exiting ..")}    
    
	log_set_level(config.log_level);          
    struct iq_frame_struct_32* iq_frame =calloc(1, sizeof(struct iq_frame_struct_32));

    /* Initializing input shared memory interface */
    struct shmem_transfer_struct* input_sm_buff = calloc(1, sizeof(struct shmem_transfer_struct));
    input_sm_buff->shared_memory_size = MAX_IQFRAME_PAYLOAD_SIZE*config.num_ch*4*2+IQ_HEADER_LENGTH;         
    input_sm_buff->io_type = 1; // Input type
    strcpy(input_sm_buff->shared_memory_names[0], DELAY_SYNC_IQ_SM_NAME_A);
    strcpy(input_sm_buff->shared_memory_names[1], DELAY_SYNC_IQ_SM_NAME_B);
    strcpy(input_sm_buff->fw_ctr_fifo_name, DELAY_SYNC_IQ_FW_FIFO);
    strcpy(input_sm_buff->bw_ctr_fifo_name, DELAY_SYNC_IQ_BW_FIFO);
	
	ret= init_in_sm_buffer(input_sm_buff);
    if (ret !=0) {FATAL_ERR("Failed to init shared memory interface")} 
	else{log_info("Shared memory interface succesfully initialized");}
	
    /* Starting IQ ethernet server */
	int run_server=1;
    while(run_server)
    {
		
		/* This function blocks until a client connects to the server */
		int * sockets = malloc(2*sizeof(int)); //[server, client] 
        iq_stream_con(sockets);        
        // TODO: Check and handle success
        
        int exit_flag =0;
        while(!exit_flag) 
        {
        	// Acquire data buffer on the shared memory interface
        	active_buff_ind = wait_buff_ready(input_sm_buff);
       	    if (active_buff_ind < 0){exit_flag = active_buff_ind; break;}
            iq_frame->header = (struct iq_header_struct*) input_sm_buff->shm_ptr[active_buff_ind];
			iq_frame->payload = ((float *) input_sm_buff->shm_ptr[active_buff_ind] )+ IQ_HEADER_LENGTH/sizeof(float);
			CHK_SYNC_WORD(check_sync_word(iq_frame->header));
			iq_frame->payload_size=iq_frame->header->cpi_length * iq_frame->header->active_ant_chs;
			//dump_iq_header(iq_frame->header);
			
			ret=send_iq_frame(iq_frame, sockets[1]);
			send_ctr_buff_free(input_sm_buff, active_buff_ind);
			if(ret !=0){log_error("Closing connection"); break;}
			
			/* Waiting for further download commands on the Ethernet link*/
			int bytes_recieved = recv(sockets[1],eth_cmd,1024,0);
			eth_cmd[bytes_recieved] = '\0';
			if (strcmp(eth_cmd, "IQDownload") !=0){exit_flag=1;}       
       }
        iq_stream_close(sockets);
    }
	destory_sm_buffer(input_sm_buff);
	log_info("DAQ chain IQ server has exited.");
}
