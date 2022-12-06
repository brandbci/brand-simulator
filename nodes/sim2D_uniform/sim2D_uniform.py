import gc
import logging
import time
import numpy as np
from brand import BRANDNode


class Simulator2D(BRANDNode):

    def __init__(self):

        super().__init__()

        self.n_neurons = self.parameters['n_neurons']
        self.max_v = self.parameters['max_v']
        if 'click_enabled' in self.parameters:
            self.click_enabled = self.parameters['click_enabled']
        else:
            self.click_enabled = 1
        self.in_stream = self.parameters['in_stream']
        self.max_samples = self.parameters['max_samples']
        self.mod_amp = self.parameters['mod_amp']

        self.max_v_mag = np.sqrt(2) * self.max_v

        self.i = np.uint32(0)
        self.i_in = np.uint32(0)

    def build(self):
        np.random.seed(42)

        #self.fr_min = np.random.uniform(size=(self.n_neurons, 1)) * 20.0

        #self.fr_max = np.random.uniform(
        #    size=(self.n_neurons, 1)) * (100.0 - self.fr_min) + self.fr_min

        #self.fr_mean = 0.5 * (self.fr_max + self.fr_min)
        #self.fr_mod = 0.5 * (self.fr_max - self.fr_min)
        self.fr_mod = self.mod_amp * np.ones(size=(self.n_neurons, 1))
        self.fr_mean = self.fr_mod

        self.click_tuning = np.random.uniform(size=(self.n_neurons,
                                                    1)) * (100.0) - 50.0

        # generate directional tuning vectors
        c = np.random.uniform(size=(self.n_neurons, 2)) * 2 - 1
        self.c = c / np.sqrt((c**2).sum(axis=1, keepdims=True))

        logging.info('Firing rates parameters initiated for '
                     f'{self.n_neurons} neurons')

    def run(self):

        self.build()

        self.mouse_data = np.zeros((2, 1), dtype=np.int16)
        self.mouse_click = 0
        self.mouse_clipped = np.zeros_like(self.mouse_data, dtype=np.float32)
        self.x_t = np.zeros_like(self.mouse_clipped, dtype=np.float32)

        self.rates = np.zeros((self.n_neurons, 1), dtype=np.float32)

        logging.info(f'Publishing firing rates for {self.n_neurons} neurons')

        self.last_id = '$'

        self.sample = {
            # time at which the output is written
            'ts': np.uint64(time.monotonic_ns()).tobytes,
            'rates': self.rates.tobytes(),
            'i': self.i.tobytes(),
            'i_in': self.i_in.tobytes()
        }

        # send samples to Redis
        while True:
            # block for samples from the mouse stream
            self.get_mouse_data()

            # compute intended velocity from mouse data
            self.mouse_data[1] = -self.mouse_data[1]
            self.mouse_clipped[:] = np.clip(self.mouse_data, -self.max_v,
                                            self.max_v)
            self.x_t[:] = self.mouse_clipped / self.max_v_mag

            # compute firing rates
            self.rates[:] = self.fr_mod * (self.c @ self.x_t) + self.fr_mean
            self.rates = self.rates + self.mouse_click * self.click_tuning * self.click_enabled
            self.rates = np.clip(self.rates, 0, None)

            # send samples to Redis
            self.sample['i'] = self.i.tobytes()
            self.sample['i_in'] = self.i_in
            self.sample['rates'] = self.rates.astype(np.float32).tobytes()
            self.sample['ts'] = np.uint64(time.monotonic_ns()).tobytes()

            self.r.xadd('firing_rates',
                        self.sample,
                        maxlen=self.max_samples,
                        approximate=True)

            self.i += np.uint32(1)

    # Getting data from Redis
    def get_mouse_data(self):
        self.reply = self.r.xread(streams={self.in_stream: self.last_id},
                                  count=1,
                                  block=0)

        self.cursorFrame = self.reply[0][1][0]
        self.last_id = self.cursorFrame[0]

        self.i_in = int.from_bytes(self.cursorFrame[1][b'index'],
                                   "little",
                                   signed=True)
        self.mouse_data[:] = np.frombuffer(self.cursorFrame[1][b'samples'],
                                           np.int16)[:2, None]
        self.mouse_click = np.frombuffer(self.cursorFrame[1][b'samples'],
                                         np.int16)[2, None]


if __name__ == "__main__":
    gc.disable()

    # setup
    simulator2D = Simulator2D()

    # main
    simulator2D.run()

    gc.collect()
