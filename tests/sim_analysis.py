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

logger = logging.getLogger(__name__)
coloredlogs.install(level='DEBUG')
logger.setLevel(logging.DEBUG)

# %% 
# Start Redis 

# SAVE_DIR = '/samba/data/sim/2023-03-14/RawData'
# RDB_DIR = os.path.join(SAVE_DIR,'RDB')
# RDB_FILENAME = 'sim_230314_008.rdb'
REDIS_IP = '127.0.0.1'
REDIS_PORT = 18000

# redis_command = ['redis-server', '--bind', REDIS_IP, '--port', str(REDIS_PORT)]
# redis_command.append('--dbfilename')
# redis_command.append(RDB_FILENAME)
# redis_command.append('--dir')
# redis_command.append(RDB_DIR)

# print('Starting redis: ' + ' '.join(redis_command))

# proc = subprocess.Popen(redis_command, stdout=subprocess.PIPE)
# redis_pid = proc.pid

# try:
#     out, _ = proc.communicate(timeout=1)
#     if out:
#         print(out.decode())
#     if 'Address already in use' in str(out):
#         print("Could not run redis-server (address already in use). Check if a Redis server is already running on that port. Aborting.")
#         exit(1)
#     else:
#         print("Launching redis-server failed for an unknown reason, check supervisor logs. Aborting.")
#         exit(1)
# except subprocess.TimeoutExpired:  # no error message received
#     print('Redis-server is running.')

r = redis.Redis(host=REDIS_IP, port=REDIS_PORT)

# busy_loading = True 
# while busy_loading:
#     try:
#         print(f"Streams in database: {r.keys('*')}")
#         busy_loading = False
#     except redis.exceptions.BusyLoadingError:
#         print('Redis is busy loading dataset in memory')
#         time.sleep(1)

# %%
# Load stream data
decoded_streams  = {}

if b'firing_rates' in r.keys('*'):
    stream_data = r.xrange(b'firing_rates')
    out = [None] * len(stream_data)
    for i, (entry_id, entry_data) in tqdm(enumerate(stream_data)):
        entry_dec = {}
        for key, val in entry_data.items():
            if key.decode() == 'rates':
                dat = np.frombuffer(val, dtype=np.float32)
                entry_dec[key.decode()] = dat[0] if dat.size == 1 else dat
            # if key.decode() == 'ts':
            #     dat = np.frombuffer(val, dtype=np.uint64)
            #     entry_dec[key.decode()] = dat[0] if dat.size == 1 else dat
            # elif key.decode() == 'sync':
            #     sync_dict = json.loads(val)
            #     for sync_key, sync_val in sync_dict.items():
            #         entry_dec[sync_key] = sync_val
            # elif key.decode() == 'samples':
            #     dat = np.frombuffer(val, dtype=np.float64)
            #     entry_dec[key.decode()] = dat[0] if dat.size == 1 else dat
            # elif key.decode() == 'thresholds':
            #     dat = np.frombuffer(val, dtype=np.float64)
            #     entry_dec[key.decode()] = dat[0] if dat.size == 1 else dat
        out[i] = entry_dec
    decoded_streams['firing_rates'] = out

fr_df = pd.DataFrame(decoded_streams['firing_rates'])

if b'spike_gen_1' in r.keys('*'):
    stream_data = r.xrange(b'spike_gen_1')
    out = [None] * len(stream_data)
    for i, (entry_id, entry_data) in tqdm(enumerate(stream_data)):
        entry_dec = {}
        for key, val in entry_data.items():
            if key.decode() == b'continuous':
                dat = np.frombuffer(val, dtype=np.float32)
                entry_dec[key.decode()] = dat[0] if dat.size == 1 else dat
            # if key.decode() == 'ts':
            #     dat = np.frombuffer(val, dtype=np.uint64)
            #     entry_dec[key.decode()] = dat[0] if dat.size == 1 else dat
            # elif key.decode() == 'sync':
            #     sync_dict = json.loads(val)
            #     for sync_key, sync_val in sync_dict.items():
            #         entry_dec[sync_key] = sync_val
            # elif key.decode() == 'samples':
            #     dat = np.frombuffer(val, dtype=np.float64)
            #     entry_dec[key.decode()] = dat[0] if dat.size == 1 else dat
            # elif key.decode() == 'thresholds':
            #     dat = np.frombuffer(val, dtype=np.float64)
            #     entry_dec[key.decode()] = dat[0] if dat.size == 1 else dat
        out[i] = entry_dec
    decoded_streams['continuous'] = out

spk1_df = pd.DataFrame(decoded_streams['continuous'])

# %%

fr_arr = np.array(fr_df['rates'].values.tolist())
# %%

plt.plot(fr_arr[:,0])

# %%
