/*
 *
 * Description :
 * Hardware-agnostic inter-module message structure and shared DAQ chain
 * error codes/macros. Split out of rtl_daq.h so that non-RTL-SDR acquisition
 * backends (e.g. usrp_daq.cc) don't need to pull in <rtl-sdr.h>.
 *
 * Project : HeIMDALL DAQ Firmware
 * License : GNU GPL V3
 *
 * Copyright 2026 en7
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef IM_MSG_H
#define IM_MSG_H

#ifdef __cplusplus
extern "C" {
#endif

#define ERR_IQFRAME_WRITE     10
#define ERR_IQFRAME_READ      11
#define ERR_CMD_READ          12
#define ERR_IQFRAME_SYC_WORD  13
#define ERR_DATA_PIPE_CLOSE   14
#define ERR_CTR_THREAD_READ   15

#define CHK_SYNC_WORD(r)   if(r != 0)     {exit_flag = ERR_IQFRAME_SYC_WORD; break;}
#define CHK_FR_WRITE(r, e) if(r != e)     {exit_flag = ERR_IQFRAME_WRITE;    break;}
#define CHK_FR_READ(r, e)  if(r != e)     {exit_flag = ERR_IQFRAME_READ;     break;}
#define CHK_CMD_READ(r, e) if(r != e)     {exit_flag = ERR_CMD_READ;         break;}
#define CHK_DATA_PIPE(fd)  if(feof(fd))   {exit_flag = ERR_DATA_PIPE_CLOSE;  break;}
#define CHK_CTR_READ(r, e) if(r != e)     {exit_flag = ERR_CTR_THREAD_READ;}

#define MAX_IQFRAME_PAYLOAD_SIZE 8388608 // 2^23[sample] per channel
//Should be greather than the cpi_size in the daq_chain_config.ini
void error_code_log(int exit_flag)
/*
 * Dump out error codes
 *
 */
{
    switch (exit_flag)
    {
    case ERR_IQFRAME_WRITE:
        log_error("IQ frame sending failed");
        break;
    case ERR_IQFRAME_READ:
        log_fatal("IQ header read error ");
        break;
    case ERR_IQFRAME_SYC_WORD:
        log_fatal("IQ frame sync word check failed");
        break;
    case ERR_DATA_PIPE_CLOSE:
        log_fatal("Unexpected data pipe close");
        break;
    case ERR_CMD_READ:
        log_fatal("Command read error");
        break;
    default:
        break;
    }

}

// HeIMDALL DAQ inter-modul message structure
struct hdaq_im_msg_struct {
    // Total length: 128 byte
    uint8_t source_module_identifier;
    char command_identifier;
    uint8_t parameters[126];
};

struct circ_buffer_struct {
    uint8_t *iq_circ_buffer;  // Stores raw sample bytes (bit depth is acquisition-backend dependent)
};

#ifdef __cplusplus
}
#endif

#endif /* IM_MSG_H */
