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
from redis import Redis
from redis.exceptions import ConnectionError

class RedisGetData(BRANDNode):
    def __init__(self):
        
        super().__init__()
  
        self.redis_ext_host = self.parameters['redis_ext_host']
        self.redis_ext_port = self.parameters['redis_ext_port']
        self.input_stream = self.parameters['input_stream']
        self.input_fields = self.parameters['input_fields']
        #self.input_dtypes = self.parameters['input_dtypes']
        self.max_samples = self.parameters['max_samples']

        try:
            self.r_ext = Redis(self.redis_ext_host, self.redis_ext_port, retry_on_timeout=True)
            print(f"[{self.NAME}] Redis connection established on external host:"
                    f" {self.redis_ext_host}, port: {self.redis_ext_port}")
        except ConnectionError as e:
            logging.info(f"Error with external Redis connection, check again: {e}")
            sys.exit(1)

        self.last_id = '$'
        self.output_entry = {}

        self.i = np.uint32(0)


    def terminate(self, sig, frame):
        self.r_ext.close()
        super().terminate(sig, frame)


    def run(self):

        logging.info(f'Receiving stream data from external Redis server...')

        while True:

            try:
                # Read stream data from exteral Redis server
                self.reply = self.r_ext.xread(streams={self.input_stream: self.last_id}, count=1, block=0)

                self.dataFrame = self.reply[0][1][0]
                self.last_id = self.dataFrame[0]

                #self.output_entry = {}
                #for i, field in enumerate(self.input_fields):
                #    self.output_entry[field.encode()] = np.frombuffer(self.dataFrame[1][field.encode()], dtype=self.input_dtypes[i])[0]
                self.output_entry = self.dataFrame[1]

                #logging.debug(f'output_entry: {self.output_entry}')

                # Write stream data to redis
                self.r.xadd(self.input_stream, self.output_entry, maxlen=self.max_samples, approximate=True)
                
                self.i += np.uint32(1)

            except ConnectionError as e:
                logging.info(f"Error with external Redis connection: {e}")
                time.sleep(1)


if __name__ == "__main__":
    gc.disable()
    
    # setup
    redis_get_data = RedisGetData()

    # main
    redis_get_data.run()

    gc.collect()
