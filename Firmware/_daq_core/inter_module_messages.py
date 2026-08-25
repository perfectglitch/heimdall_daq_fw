from struct import pack

def pack_msg_resync(module_identifier):
    """
        Prepares the byte array of an inter-module ZMQ message requesting a
        USRP resync (device close/reopen + PPS re-sync) at the current
        frequency, with no retune. USRP-only ('y' handler in usrp_daq.cc).

        Deliberately a distinct command from pack_msg_rf_tune(), not just
        pack_msg_rf_tune() called with the unchanged frequency: usrp_daq.cc
        used to infer "resync" from a same-frequency 'c', but the
        krakensdr_doa web UI also sends its configured frequency as an
        ordinary 'c' on every connect (including right after a fresh boot),
        which routinely matches the current frequency too -- that made the
        inference misfire into an unnecessary, and once observed to hang,
        device reopen moments after a perfectly good boot. A same-frequency
        'c' is now a no-op on the receiving end; only this command forces
        the resync.

        Parameters:
        -----------
            :param: module_identifier: Source module id

            :type: module_identifier: int

        Return:
        -------
            Assembled message structure in byte array
    """
    msg_length = 128  # Total message length 128 byte
    msg_byte_array  = pack("b", module_identifier)  # 1 byte
    msg_byte_array += 'y'.encode('ascii')  # 1 byte
    for m in range(msg_length - 1 - 1):
        msg_byte_array += pack('b', 0)

    return msg_byte_array

def pack_msg_reconfiguration(module_identifier,center_frequency, sample_rate, gain):
    """
        Prepares the byte array of an inter-module ZMQ message for tunner reconfiguration.
        
        Parameters:
        -----------
            :param: module_identifier: Source module id
            :param: center_frequency: New RF center frequency, specified in [Hz]
            :param: sample_rate: New ADC sampling frequency, specified in [Hz]
            :param: gain: New Gain value (Will be unique for all tuners)

            :type: module_identifier: int
            :type: center_frequency: int
            :type: sample_rate: int
            :type: gain: int

        Return:
        -------
            Assembled message structure in byte array 
    """    
    msg_length = 128 # Total message length 128 byte
    msg_byte_array  = pack("b", module_identifier) # 1byte
    msg_byte_array += 'r'.encode('ascii') # 1 byte
    # center_frequency is 8 byte ('Q') -- 4 byte ('I') tops out at ~4.295GHz,
    # well within common bands (e.g. 5.8GHz ISM); see pack_msg_rf_tune.
    msg_byte_array += pack('QII', center_frequency, sample_rate, gain) # 16 byte
    for m in range(msg_length-1-1-16):
        msg_byte_array +=pack('b',0)

    return msg_byte_array

def pack_msg_rf_tune(module_identifier,center_frequency):
    """
        Prepares the byte array of an inter-module ZMQ message for Radio Frequency
        tunning.
        
        Parameters:
        -----------
            :param: module_identifier: Source module id
            :param: center_frequency: New RF center frequency, specified in [Hz]

            :type: module_identifier: int
            :type: center_frequency: int

        Return:
        -------
            Assembled message structure in byte array 
    """    
    msg_length = 128 # Total message length 128 byte
    msg_byte_array  = pack("b", module_identifier) # 1byte
    msg_byte_array += 'c'.encode('ascii') # 1 byte
    # 8 byte ('Q'), not 4 ('I') -- 'I' tops out at ~4.295GHz, which is below
    # common bands like the 5.8GHz ISM band (confirmed live: struct.error
    # crashed hw_controller.py's control loop the instant it tried to relay
    # a 5.8GHz request here).
    msg_byte_array += pack('Q', center_frequency) # 8 byte
    for m in range(msg_length-1-1-8):
        msg_byte_array +=pack('b',0)

    return msg_byte_array

def pack_msg_set_gain(module_identifier, gains):
    """
        Prepares the byte array of an inter-module ZMQ message for receiver gain change.
        
        Parameters:
        -----------
            :param: module_identifier: Source module id
            :param: gain: New Gain values (Can be different for the individual receivers)

            :type: module_identifier: int
            :type: gains: list of ints values [gain_for_ch1, gain_for_ch2, ..]

        Return:
        -------
            Assembled message structure in byte array 
    """    
    msg_length = 128 # Total message length 128 byte
    msg_byte_array  = pack("b", module_identifier) # 1byte
    msg_byte_array += 'g'.encode('ascii') # 1 byte
    for gain in gains:
        msg_byte_array += pack('I',  gain) # 4 byte
    for m in range(msg_length-1-1-len(gains)*4):    
        msg_byte_array +=pack('b',0)    
    return msg_byte_array


def pack_msg_enable_agc(module_identifier):
    """
        Prepares the byte array of an inter-module ZMQ message for enabling
        automatic gain control.

        Parameters:
        -----------
            :param: module_identifier: Source module id

        Return:
        -------
            Assembled message structure in byte array
    """
    msg_length = 128 # Total message length 128 byte
    msg_byte_array  = pack("b", module_identifier) # 1byte
    msg_byte_array += 'a'.encode('ascii') # 1 byte
    for _ in range(msg_length-1-1):
        msg_byte_array +=pack('b',0)

    return msg_byte_array


def pack_msg_noise_source_ctr(module_identifier, state):
    """
        Prepares the byte array of an inter-module ZMQ message for internal noise source control
        
        Parameters:
        -----------
            :param: module_identifier: Source module id
            :param: state: True will turn on the noise source, False will turn off the noise source

            :type: module_identifier: int
            :type: stae: Boolean

        Return:
        -------
            Assembled message structure in byte array 
    """    
    msg_length = 128 # Total message length 128 byte
    msg_byte_array  = pack("b", module_identifier) # 1byte
    msg_byte_array += 'n'.encode('ascii') # 1 byte
    if state:
        msg_byte_array += pack('b',1) # 1 byte
    else:
        msg_byte_array += pack('b',0) # 1 byte
    for m in range(msg_length-1-1-1):    
        msg_byte_array +=pack('b',0)    
    return msg_byte_array

def pack_msg_sample_freq_tune(module_identifier, fs_ppm_offsets):
    """
        Prepares the byte array of an inter-module ZMQ message for sampling frequency ppm offset seting.

        Parameters:
        -----------
            :param: module_identifier: Source module id
            :param: fs_ppm_offsets: List of sampling frequency offset for the individual receivers channels.

            :type: module_identifier: int
            :type: fs_ppm_offsets: list of float values [fs offset for ch1, fs offset for ch 2]

        Return:
        -------
            Assembled message structure in byte array
    """    
    msg_length = 128 # Total message length 128 byte
    msg_byte_array  = pack("b", module_identifier) # 1byte
    msg_byte_array += 's'.encode('ascii') # 1 byte
    for fs_offset in fs_ppm_offsets:
            msg_byte_array += pack('f',  fs_offset) # 4 byte
    for m in range(msg_length-1-1-len(fs_ppm_offsets)*4):    
        msg_byte_array +=pack('b',0)    
       
    return msg_byte_array
    