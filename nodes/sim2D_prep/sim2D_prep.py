
import gc
import logging
import os
import signal
import sys
import time
import numpy as np
from brand import BRANDNode

class Simulator2D(BRANDNode):
    def __init__(self):
        
        super().__init__()

        self.n_neurons = self.parameters['n_neurons']
        self.max_v = self.parameters['max_v']
        self.in_stream = self.parameters['in_stream']
        self.max_samples = self.parameters['max_samples']

        self.i = np.uint32(0) 
        self.i_in = np.uint32(0)  

        

    def build(self):
        np.random.seed(42)

        self.fr_min = np.random.uniform(size=(self.n_neurons, 1)) * 20.0

        self.fr_max = np.random.uniform(
            size=(self.n_neurons, 1)) * (100.0 - self.fr_min) + self.fr_min

        self.fr_mean = 0.5 * (self.fr_max + self.fr_min)
        self.fr_mod = 0.5 * (self.fr_max - self.fr_min)

        self.click_tuning = np.random.uniform(size=(self.n_neurons, 1)) * (100.0) - 50.0

        # generate directional tuning vectors
        c = np.random.uniform(size=(self.n_neurons, 4)) * 2 - 1
        self.c = c / np.sqrt((c**2).sum(axis=1, keepdims=True))

        logging.info(f'Firing rates parameters initiated for {self.n_neurons} neurons')

        self.cursor_pos_start = [0,0]
        self.target_pos = [0,0]
        self.target_state = 0
        self.target_state_last = 0

        self.moving = 0

        self.preparatory_state = 0 # 0: rest, 1: preparation, 2: movement 
        self.p_t = np.array([[0],[0]])
        self.t_t = 0 # preparatory activity on
        self.t_t_duration = 0
        self.v_t_duration = 0

        self.max_p_t = 1000
        self.max_p_t_mag = 1000*10 #np.sqrt(2) * self.max_p_t

        self.max_v_mag = np.sqrt(2) * self.max_v

        self.x_t = np.zeros((4,1))

    def run(self):

        self.build()

        self.mouse_data = np.zeros((2, 1), dtype=np.int16)
        self.mouse_click = 0
        self.mouse_clipped = np.zeros_like(self.mouse_data, dtype=np.float32)
        self.v_t = np.zeros_like(self.mouse_clipped, dtype=np.float32)
        self.p_t_clipped = np.zeros_like(self.p_t, dtype=np.float32)

        self.rates = np.zeros((self.n_neurons, 1), dtype=np.float32)

        logging.info(f'Publishing firing rates for {self.n_neurons} neurons...')

        self.last_id = '$'

        self.sample = {
            'ts_start': time.monotonic(), # time at which we start XREAD
            'ts': time.monotonic(), # time at which the output is written
            'ts_end': time.monotonic(), # time at which XADD is complete
            'rates': self.rates.tobytes(),
            'prep_subspace': self.x_t[0:2,:].tobytes(),
            'move_subspace': self.x_t[2:4,:].tobytes(),
            'target_state': self.target_state,
            'moving': self.moving,
            'i': self.i.tobytes(),   
            'i_in': self.i_in.tobytes() 
        }

        # send samples to Redis
        while True:
            
            # block for samples from the mouse stream
            self.get_mouse_data()

            # read latest tatget & cursor values from respective streams
            self.get_target_data()
            self.get_cursor_data()

            # compute intended velocity from mouse data
            self.mouse_data[1] = -self.mouse_data[1]
            self.mouse_clipped[:] = np.clip(self.mouse_data, -self.max_v,
                                        self.max_v)
            self.v_t[:] = self.mouse_clipped / self.max_v_mag

            # update preparatory state (p_t)
            self.update_preparatory_state()
            self.p_t_clipped = self.p_t / self.max_p_t_mag

            # copy p_t and v_t to state
            self.x_t[0:2,:] = self.p_t_clipped
            self.x_t[2:4,:] = self.v_t

            # compute firing rates
            self.rates[:] = self.fr_mod * (self.c @ self.x_t) + self.fr_mean
            self.rates = self.rates + self.mouse_click * self.click_tuning 
            self.rates = np.clip(self.rates, 0, None)

            # send samples to Redis
            self.sample['i'] = self.i.tobytes()
            self.sample['i_in'] = self.i_in
            self.sample['rates'] = self.rates.astype(np.float32).tobytes()
            self.sample['ts'] = time.monotonic()
            self.sample['prep_subspace'] = self.x_t[0:2,:].tobytes()
            self.sample['move_subspace'] = self.x_t[2:4,:].tobytes()
            self.sample['target_state'] = self.target_state
            self.sample['moving'] = self.moving

            self.r.xadd('firing_rates', self.sample, maxlen=self.max_samples, approximate=True)
            
            self.sample['ts_end'] = time.monotonic()
            
            self.i += np.uint32(1)

        logging.info('Exiting')

    def update_preparatory_state(self):
        
        if self.target_state != self.target_state_last and self.target_state != 2:
            self.t_t = 1
            self.t_t_duration = 0

        #print(f"t_t: {self.t_t} -- t_s: {self.target_state} -- t_s_l: {self.target_state_last} -- v_t: {np.linalg.norm(self.v_t)}")

        self.target_state_last = self.target_state

        if np.linalg.norm(self.v_t) > 0.2: 
            self.v_t_duration += 1
            if self.v_t_duration > 2:
                self.moving = 1
        else:
            self.v_t_duration = 0
            self.moving = 0


        if self.moving == 1 and self.t_t_duration > 200/5: # prep activity at least 200ms
            self.t_t = 0

        self.t_t_duration += 1

        u_t = np.array([[self.target_pos[0]-self.cursor_pos_start[0]],[self.target_pos[1]-self.cursor_pos_start[1]]])

        J_prep = np.array([[-0.09, 0],[0, -0.09]])

        #print(J_prep.shape)
        #print(self.p_t.shape)

        self.p_t = self.p_t + (0.005/0.010)*(J_prep @ self.p_t + self.t_t*u_t)

        #print(f"t_t: {t_t} -- u_t: {u_t} -- p_t: {self.p_t}")

    # get latest cursor data from redis
    def get_cursor_data(self):
        self.reply = self.r.xrevrange('cursorData', max='+', min='-', count=1)
        
        if len(self.reply) > 0:
            self.dataDict_cursor = self.reply[0][1] 

            self.cursor_pos_start[0] = float(self.dataDict_cursor[b'X_start'])
            self.cursor_pos_start[1] = float(self.dataDict_cursor[b'Y_start'])

    # get latest target data from redis
    def get_target_data(self):
        self.reply = self.r.xrevrange('targetData', max='+', min='-', count=1)
        
        if len(self.reply) > 0:
            self.dataDict_target = self.reply[0][1]

            self.target_pos[0] = float(self.dataDict_target[b'X'])
            self.target_pos[1] = float(self.dataDict_target[b'Y'])

            self.target_state = int(self.dataDict_target[b'state']) 

        #print(self.target_pos)
        #print(self.target_on)
  

    # Getting data from Redis
    def get_mouse_data(self):
        self.sample['ts_start'] = time.monotonic()
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
