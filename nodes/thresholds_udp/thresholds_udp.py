
import gc
import logging
import os
import signal
import sys
import time
import socket
import numpy as np
from brand import BRANDNode

class SpikeGenerator(BRANDNode):
    def __init__(self):

        super().__init__()

        self.fr_sample_rate = self.parameters['fr_sample_rate']
        self.sample_rate = self.parameters['sample_rate']
        self.random_seed = self.parameters['random_seed']
        self.max_samples = self.parameters['max_samples']
        self.fr_stream = self.parameters['input_stream']
        self.output_stream = self.parameters['output_stream']
        self.n_neurons = self.parameters['n_neurons']

        self.UDP_IP = self.parameters['udp_ip']
        self.UDP_PORT = self.parameters['udp_port']
        self.UDP_INTERFACE = self.parameters['udp_interface']

        self.period = 1/self.sample_rate
        self.fr_iterations = int(self.sample_rate/self.fr_sample_rate)
        
        logging.info(f'Sampling period: {self.period}')

        self.i = 0  # initialize sample # variable

        self.last_id = '$'
        self.last_time = time.monotonic()

        self.rates = None
        self.rates_sub = None
        self.rates_rep = None
        self.modified_rates = None
        self.spike_counts = None
        self.spikes = None

        self.refractory_period = 60 # refractory period of 2ms
        self.spike_last_samples = self.refractory_period

        #self.buffer1k_spikes = np.zeros((self.fr_iterations, self.n_neurons))
        self.buffer1k_spikes = np.zeros((1, self.n_neurons))

        self.sample = {
            'ts_start': float(),  # time at which we start XREAD
            'ts_in': float(),  # time at which the input is received
            'ts': float(),  # time at which the output is written
            'ts_end': float(),  # time at which XADD is complete
            'i': int(),
            'i_in': int()
        }
 
        self.sock = socket.socket(
            socket.AF_INET,  # Internet
            socket.SOCK_DGRAM)  # UDP

        if self.UDP_INTERFACE != None:
            self.sock.setsockopt(
                socket.SOL_SOCKET,
                socket.SO_BINDTODEVICE,
                self.UDP_INTERFACE.encode(),
            )

        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1) # allow broadcast

        logging.info(f'Broadcasting to {self.UDP_IP}:{self.UDP_PORT} on interface \'{self.UDP_INTERFACE}\'')

        self.xPC_clock = 0
        self.nsp1_clock = 0
        self.nsp2_clock = 0
        self.message = bytearray(1000)

    def send_udp(self):
        self.xPC_clock = np.uint32(self.i) 
        self.nsp1_clock = np.uint32(self.i*30)
        self.nsp2_clock = np.uint32(self.i*30)  

        self.message[0:4] = self.xPC_clock.tobytes() 
        self.message[4:8] = self.nsp1_clock.tobytes() 
        self.message[8:12] = self.nsp2_clock.tobytes() 
        self.message[12:12+self.n_neurons] = self.buffer1k_spikes.astype(np.int8).tobytes() 

        self.sock.sendto(self.message, (self.UDP_IP, self.UDP_PORT))      

    def build(self):

        np.random.seed(self.random_seed)

    def run(self):

        logging.info(f'Publishing continuous neural data for {self.n_neurons} channels...')

        self.build()

        self.last_time = time.monotonic()

        # send samples to Redis
        while True:
            self.sample['ts_start'] = time.monotonic()
            self.streams = self.r.xread(streams={self.fr_stream: self.last_id}, block=0, count=1)

            self.last_time = time.monotonic()

            self.sample['ts_in'] = self.last_time
            
            self.stream_name, self.stream_entries = self.streams[0]
            self.entry_id, self.entry_dict = self.stream_entries[0]
            self.last_id = self.entry_id

            # TODO: pre-allocate self.rates
            self.rates = np.frombuffer(self.entry_dict[b'rates'], dtype=np.float32)            

            self.sample['i_in'] = self.entry_dict[b'i_in']

            for s in range(0, self.fr_iterations):

                self.buffer1k_spikes = np.random.binomial(1, self.rates/ self.sample_rate)

                self.sample['i'] = self.i
                self.sample['thresholds'] = self.buffer1k_spikes.astype(np.int8).tobytes()                    

                # wait before sending sample (should replace with clock_nanosleep)
                while time.monotonic() < self.last_time + s*self.period: 
                    time.sleep(1e-6)

                self.sample['ts'] = time.monotonic()
                self.r.xadd(self.output_stream, self.sample, maxlen=self.max_samples, approximate=True)
                
                self.sample['ts_end'] = time.monotonic()
                
                self.send_udp()

                self.i += 1


        logging.info('Exiting')



if __name__ == "__main__":
    gc.disable()

    # setup
    spike_generator = SpikeGenerator()

    # main
    spike_generator.run()

    gc.collect()
