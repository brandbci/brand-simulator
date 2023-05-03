# %% 
# Imports

import os
import sys
import time
import redis
import yaml
import json
import logging
import coloredlogs
import subprocess
from tqdm.auto import tqdm
import numpy as np
import pandas as pd
import matplotlib.pyplot as plt
from brand.timing import timespecs_to_timestamps, timevals_to_timestamps
from struct import unpack

logger = logging.getLogger(__name__)
coloredlogs.install(level='DEBUG')
logger.setLevel(logging.DEBUG)

# %% 
# Connect to Redis

REDIS_IP = '192.168.30.8'
REDIS_PORT = 18000

r = redis.Redis(host=REDIS_IP, port=REDIS_PORT)

# %%
# Load supergraph data

supergraph_dict = json.loads(r.xrevrange(b'supergraph_stream', count=1)[0][1][b'data'])

N_CHANNELS = int(supergraph_dict['nodes']['spike_gen_1']['parameters']['n_neurons'])
N_CHANNELS_ARRAY = int(N_CHANNELS)

# %%
# Load stream data

decoded_streams  = {}

if b'mouse_vel' in r.keys('*'):
    stream_data = r.xrange(b'mouse_vel')
    out = [None] * len(stream_data)
    for i, (entry_id, entry_data) in tqdm(enumerate(stream_data)):
        mouse_dict = {
            'i': int.from_bytes(entry_data[b'index'], "little", signed=True),
            'mouse_data': unpack('3h', entry_data[b'samples'])
        }
        entry_dec = {
            'i_in': mouse_dict['i'],
            'ts_mouse': timespecs_to_timestamps(entry_data[b'timestamps'])[0],
            'mouse_vel_x': mouse_dict['mouse_data'][0],
            'mouse_vel_y': mouse_dict['mouse_data'][1],
        }
        out[i] = entry_dec
    decoded_streams['mouse_vel'] = out

mouse_df = pd.DataFrame(decoded_streams['mouse_vel'])
mouse_df.set_index('i_in', inplace=True)

if b'firing_rates' in r.keys('*'):
    stream_data = r.xrange(b'firing_rates')
    out = [None] * len(stream_data)
    for i, (entry_id, entry_data) in tqdm(enumerate(stream_data)):
        entry_dec = {
            'ts_start_fr': float(entry_data[b'ts_start']),
            'ts_fr': float(entry_data[b'ts']),
            'ts_end_fr': float(entry_data[b'ts_end']),
            'rates': np.frombuffer(entry_data[b'rates'], dtype=np.float32),
            'prep_subspace': np.frombuffer(entry_data[b'prep_subspace'], dtype=np.float),
            'move_subspace': np.frombuffer(entry_data[b'move_subspace'], dtype=np.float),
            'speed_subspace': np.frombuffer(entry_data[b'speed_subspace'], dtype=np.float),
            'target_state': np.frombuffer(entry_data[b'target_state'], dtype=np.int32)[0],
            'moving': float(entry_data[b'moving']),
            't_t': float(entry_data[b't_t']),
            'i_fr': np.frombuffer(entry_data[b'i'], dtype=np.int32)[0],
            'i_in': int(entry_data[b'i_in']),
        }
        out[i] = entry_dec
    decoded_streams['firing_rates'] = out

fr_df = pd.DataFrame(decoded_streams['firing_rates'])
fr_df.set_index('i_in', inplace=True)

if b'spike_gen_1' in r.keys('*'):
    stream_data = r.xrange(b'spike_gen_1')
    out = [None] * len(stream_data)
    for i, (entry_id, entry_data) in tqdm(enumerate(stream_data)):
        entry_dec = {
            'ts_start_spk1': float(entry_data[b'ts_start']),
            'ts_in_spk1': float(entry_data[b'ts_in']),
            'ts_spk1': float(entry_data[b'ts']),
            'ts_end_spk1': float(entry_data[b'ts_end']),
            'i_spk1': int(entry_data[b'i']),
            'i_in': int(entry_data[b'i_in']),
            'continuous_spk1': np.frombuffer(entry_data[b'continuous'], dtype=np.int16).reshape(-1,N_CHANNELS_ARRAY),
            'thresholds_spk1': np.frombuffer(entry_data[b'thresholds'], dtype=np.int8),
        }
        out[i] = entry_dec
    decoded_streams['spike_gen_1'] = out

spk1_df = pd.DataFrame(decoded_streams['spike_gen_1'])
spk1_df.set_index('i_in', inplace=True)

if b'spike_gen_2' in r.keys('*'):
    stream_data = r.xrange(b'spike_gen_2')
    out = [None] * len(stream_data)
    for i, (entry_id, entry_data) in tqdm(enumerate(stream_data)):
        entry_dec = {
            'ts_start_spk2': float(entry_data[b'ts_start']),
            'ts_in_spk2': float(entry_data[b'ts_in']),
            'ts_spk2': float(entry_data[b'ts']),
            'ts_end_spk2': float(entry_data[b'ts_end']),
            'i_spk2': int(entry_data[b'i']),
            'i_in': int(entry_data[b'i_in']),
            'continuous_spk2': np.frombuffer(entry_data[b'continuous'], dtype=np.int16).reshape(-1,N_CHANNELS_ARRAY),
            'thresholds_spk2': np.frombuffer(entry_data[b'thresholds'], dtype=np.int8),
        }
        out[i] = entry_dec
    decoded_streams['spike_gen_2'] = out

spk2_df = pd.DataFrame(decoded_streams['spike_gen_2'])
spk2_df.set_index('i_in', inplace=True)

graph_df1 = spk1_df.join(fr_df.join(mouse_df)).set_index('i_spk1')
graph_df2 = spk2_df.join(fr_df.join(mouse_df)).set_index('i_spk2')

if b'cb_gen_1' in r.keys('*'):
    stream_data = r.xrange(b'cb_gen_1')
    out = [None] * len(stream_data)
    for i, (entry_id, entry_data) in tqdm(enumerate(stream_data)):
        entry_dec = {
            'ts': timespecs_to_timestamps(entry_data[b'timestamps'])[0],
        }
        out[i] = entry_dec
    decoded_streams['cb_gen_1'] = out

cb_1_df = pd.DataFrame(decoded_streams['cb_gen_1'])

if b'cb_gen_2' in r.keys('*'):
    stream_data = r.xrange(b'cb_gen_2')
    out = [None] * len(stream_data)
    for i, (entry_id, entry_data) in tqdm(enumerate(stream_data)):
        entry_dec = {
            'ts': timespecs_to_timestamps(entry_data[b'timestamps'])[0],
        }
        out[i] = entry_dec
    decoded_streams['cb_gen_2'] = out

cb_2_df = pd.DataFrame(decoded_streams['cb_gen_2'])

# %%
# Plot timing of output packets

plt.figure()
plt.plot(cb_1_df['ts'].diff())
plt.show()

plt.figure()
plt.plot(cb_2_df['ts'].diff())
plt.show()

# %%
# Close Redis

r.close()

