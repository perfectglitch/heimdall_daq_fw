/*
 * 
 * Description :
 * Various descriptor structures for the DAQ chain 
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

#include <pthread.h>
#include <rtl-sdr.h>
#include <stdint.h>
#include "log.h"
#include "im_msg.h"

struct rtl_rec_struct {
    int dev_ind, gain, agc;
    rtlsdr_dev_t *dev;
    uint8_t *buffer;   
    unsigned long long buff_ind;
    pthread_t async_read_thread;        
    uint32_t center_freq, sample_rate;
};
struct sync_buffer_struct { // Each channel has a circular buffer struct
	uint32_t delay;
	uint8_t *circ_buffer;  // Circular buffer
};

// circ_buffer_struct (used by rebuffer.c) now lives in im_msg.h, which this
// header already pulls in -- it's not RTL-specific.




