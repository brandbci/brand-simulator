#!/usr/bin/env python
# -*- coding: utf-8 -*-
# udp_send.py

import gc
import json
import logging
import os
import signal
import socket
import sys
import time
import numpy as np
from brand import BRANDNode

class UDPGetData(BRANDNode):
    def __init__(self):
        
        super().__init__()
  
        self.udp_ip = self.parameters['udp_ip']
        self.udp_port = self.parameters['udp_port']
        self.max_samples = self.parameters['max_samples']

        # Setup UDP socket
        self.sock = socket.socket(
            socket.AF_INET,     # Internet
            socket.SOCK_DGRAM)  # UDP
        
        self.sock.bind((self.udp_ip, self.udp_port))

    def terminate(self, sig, frame):
        self.sock.close()
        super().terminate(sig, frame)

    def run(self):

        logging.info(f'Receiving stream data from UDP on port {self.udp_port}...')

        while True:

            # Read from UDP
            data, addr = self.sock.recvfrom(1024)

            data_dict = json.loads(data.decode())
            for key in data_dict.keys():

                # Write stream data to redis
                self.r.xadd(str(key), data_dict[key], maxlen=self.max_samples, approximate=True)
                
            #print(f"Received data_dict keys: {data_dict.keys()}")


if __name__ == "__main__":
    gc.disable()
    
    # setup
    udp_get_data = UDPGetData()

    # main
    udp_get_data.run()

    gc.collect()
