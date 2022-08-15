
import gc
import logging
import os
import signal
import sys
import time
from scipy.signal import fftconvolve
import numpy as np
from brand import BRANDNode

class SpikeGenerator30k(BRANDNode):
    def __init__(self):

        # load parameters
        self.fr_sample_rate = self.parameters['fr_sample_rate']
        self.sample_rate = self.parameters['sample_rate']
        self.continuous_rate = self.parameters['continuous_rate']
        self.random_seed = self.parameters['random_seed']
        self.scale = self.parameters['scale']
        self.fr_stream = self.parameters['input_stream']
        self.output_stream = self.parameters['output_stream']
        self.n_neurons_total = self.parameters['n_neurons']
        self.n_start = self.parameters['n_start']
        self.n_end = self.parameters['n_end']
        self.max_samples = self.parameters['max_samples']

        # compute derived parameters
        self.period = 1/self.sample_rate
        self.fr_iterations = int(self.sample_rate/self.fr_sample_rate) # process loops per firing rate sample 
        self.ms_iterations = int(self.continuous_rate/self.sample_rate) # 30khz samples per process loop
        self.buffer_window = int(self.continuous_rate*0.001) # 1ms buffer for 30khz samples
        self.n_neurons = self.n_end - self.n_start

        logging.info(f'Sampling period: {self.period}')

        self.i = 0  # initialize sample # variable
        self.ii = 0

        self.last_id = b'$'
        self.last_time = time.monotonic()

        self.rates = None
        self.rates_sub = None
        self.rates_rep = None
        self.modified_rates = None
        self.spike_counts = None
        self.spikes = None

        self.refractory_period = 60 # refractory period of 2ms
        self.spike_last_samples = self.refractory_period

        self.buffer30k_spikes = np.zeros((self.fr_iterations*self.ms_iterations+self.buffer_window, self.n_neurons))
        self.buffer30k_continuous = np.zeros((self.fr_iterations*self.ms_iterations+self.buffer_window, self.n_neurons))
        self.buffer30k_window = np.zeros((self.buffer_window, self.n_neurons))

        self.ap = np.array([[0, -0.1, -0.2, -0.3, -0.4, -0.5, -0.6, -0.7, -0.8, 
                            -0.7, -0.6, -0.5, -0.4, -0.3, -0.2, -0.1, 0,
                            0.05, 0.1, 0.15, 0.2,
                            0.19, 0.18, 0.17, 0.16, 0.15, 0.14, 0.13, 0.12, 0.11, 0.10, 
                            0.09, 0.08, 0.07, 0.06, 0.05, 0.04, 0.03, 0.02, 0.01, 0]]).T

        self.ap = np.tile(self.ap, (1, self.n_neurons))

        self.sample = {
            'ts_start': float(),  # time at which we start XREAD
            'ts_in': float(),  # time at which the input is received
            'ts': float(),  # time at which the output is written
            'ts_end': float(),  # time at which XADD is complete
            'i': int(),
            'i_in': int(),
        }

    def build(self):

        np.random.seed(self.random_seed)

    def run(self):
            
        logging.info(f'Publishing continuous neural data for {self.n_neurons} channels: ({self.n_start} thru {self.n_end})...')

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
            self.rates = np.frombuffer(self.entry_dict[b'rates'],
                                        dtype=np.float32)
            self.rates_sub = self.rates[self.n_start:self.n_end]
            self.rate_rep = np.tile(self.rates_sub, (self.fr_iterations*self.ms_iterations,1))
            
            self.sample['i_in'] = self.entry_dict[b'i_in']

            # copy spikes from last segment of previous loop to from segment of current loop
            self.buffer30k_spikes[:self.buffer_window,:] = self.buffer30k_spikes[-self.buffer_window:,:]
            # generate spikes for samples after intial segment buffer (rates scaled to spks/30khz-window) 
            self.buffer30k_spikes[self.buffer_window:,:] = np.random.binomial(1, self.rate_rep / self.continuous_rate)
            # generate continuous data, by convolving spikes with AP waveform, and scaling voltage
            self.buffer30k_continuous = self.scale*fftconvolve(self.buffer30k_spikes, self.ap, 
                mode='full', axes=0)[:self.fr_iterations*self.ms_iterations+self.buffer_window]

            self.ii = self.buffer_window # start indexing from after buffer window
            for s in range(0, self.fr_iterations):

                self.sample['i'] = self.i
                # senf data in 'ms_iterations' (1ms) chunks
                self.sample['continuous'] = (self.buffer30k_continuous[self.ii:self.ii+self.ms_iterations,:]).astype(np.int16).tobytes()
                self.sample['thresholds'] = self.buffer30k_spikes[self.ii:self.ii+self.ms_iterations,:].sum(axis=0).astype(np.int8).tobytes()                    

                self.sample['ts'] = time.monotonic()
                self.r.xadd(self.output_stream, self.sample, maxlen=self.max_samples, approximate=True)
                
                self.sample['ts_end'] = time.monotonic()
                
                self.i += 1
                self.ii += self.ms_iterations


        logging.info('Exiting')



if __name__ == "__main__":
    gc.disable()

    # setup
    spike_generator = SpikeGenerator30k()

    # main
    spike_generator.run()

    gc.collect()
