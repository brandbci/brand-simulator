#!/usr/bin/env python
# %%
import os
import pickle
from datetime import datetime
from struct import unpack
import argparse
from redis import Redis
import json
import matplotlib.pyplot as plt
import numpy as np
import pandas as pd

from analysis_utils import timeval_to_datetime, timespec_to_timestamp

# parse input arguments
argp = argparse.ArgumentParser()
argp.add_argument('-i', '--redis_host', type=str, required=False, default='localhost')
argp.add_argument('-p', '--redis_port', type=int, required=False, default=6379)
argp.add_argument('-s', '--redis_socket', type=str, required=False)
args = argp.parse_args()

PROCESS = "analyze_simulatorLite"
redis_host = args.redis_host
redis_port = args.redis_port
redis_socket = args.redis_socket

# connect to Redis
try:
    if redis_socket:
        r = Redis(unix_socket_path=redis_socket)
        print(f"[{PROCESS}] Redis connection established on socket:"
                f" {redis_socket}")
    else:
        r = Redis(redis_host, redis_port, retry_on_timeout=True)
        print(f"[{PROCESS}] Redis connection established on host:"
                f" {redis_host}, port: {redis_port}")
except ConnectionError as e:
    print(f"[{PROCESS}] Error with Redis connection, check again: {e}")
    sys.exit(1)

# load model parameters
model_stream_entry = r.xrevrange(b'supergraph_stream', '+', '-', 1)[0]
if model_stream_entry is None:
    print(f"[{PROCESS}] No model published to supergraph_stream in Redis")
    sys.exit(1)
entry_id, entry_dict = model_stream_entry
model_data = json.loads(entry_dict[b'data'].decode())

n_neurons = model_data['nodes']['sim2D_prep']['parameters']['n_neurons']
print(f'[{PROCESS}] # of neurons: {n_neurons}')

# %%
# Load entries from mouse_vel

replies = r.xrange(b'mouse_vel')

print(f'[{PROCESS}] {len(replies)} replies in [mouse_vel]')

entries = []
for i, reply in enumerate(replies):
    entry_id, entry_dict = reply
    mouse_dict = {
        'i': int.from_bytes(entry_dict[b'index'], "little", signed=True),
        'mouse_data': unpack('3h', entry_dict[b'samples'])
    }
    entry = {
        'i': mouse_dict['i'],
        'ts_mouse': timespec_to_timestamp(entry_dict[b'timestamps']),
        'mouse_vel_x': mouse_dict['mouse_data'][0],
        'mouse_vel_y': mouse_dict['mouse_data'][1],
    }
    entries.append(entry)

graph_data = pd.DataFrame(entries)
graph_data.set_index('i', inplace=True)

print(graph_data)

# %%
# Load entries from firing_rates
replies = r.xrange(b'firing_rates')

print(f'[{PROCESS}] {len(replies)} replies in [firing_rates]')

entries = []
for i, reply in enumerate(replies):
    entry_id, entry_dict = reply
    entry = {
        'ts_start_fr': float(entry_dict[b'ts_start']),
        'ts_fr': float(entry_dict[b'ts']),
        'ts_end_fr': float(entry_dict[b'ts_end']),
        'i_in_fr': int(float(entry_dict[b'i_in'])),
        'i_fr': np.frombuffer(entry_dict[b'i'], dtype=np.uint32).item(),
        'rates': np.frombuffer(entry_dict[b'rates'], dtype=np.float32),
        'prep_subspace': np.frombuffer(entry_dict[b'prep_subspace'], dtype=np.float),
        'move_subspace': np.frombuffer(entry_dict[b'move_subspace'], dtype=np.float),
        'target_state': int(float(entry_dict[b'target_state'])),
        'moving': int(float(entry_dict[b'moving'])),
        't_t': int(float(entry_dict[b't_t']))
    }
    entries.append(entry)

fr_df = pd.DataFrame(entries)
fr_df.set_index('i_in_fr', inplace=True)

print(fr_df)

graph_data = graph_data.join(fr_df)

# %%
# Load entries from thresholdValues
replies = r.xrange(b'threshold_values')

print(f'[{PROCESS}] {len(replies)} replies in [threshold_values]')

entries = []
for i, reply in enumerate(replies):
    entry_id, entry_dict = reply

    entry = {
        'ts_start_thres': float(entry_dict[b'ts_start']),
        'ts_in_thres': float(entry_dict[b'ts_in']),
        'ts_thres': float(entry_dict[b'ts']),
        'ts_end_thres': float(entry_dict[b'ts_end']),
        'i_in_thres': int(float(entry_dict[b'i_in'])),
        'i_thres': int(float(entry_dict[b'i'])),
        'thresholds': np.frombuffer(entry_dict[b'thresholds'], dtype=np.int8),
        #'continuous1': np.frombuffer(entry_dict[b'continuous'], dtype=np.int16).reshape((-1,int(n_neurons/2)))
        #'thresholds_30k': np.frombuffer(entry_dict[b'thresholds_30k'], dtype=np.float).reshape((-1,n_neurons)),
    }
    entries.append(entry)

thres_df = pd.DataFrame(entries)
# index is the input stream's index
#thres_df.set_index('i_in_thres', inplace=True)
#graph_data = graph_data.join(thres_df, on='i_fr')


thres_df.set_index('i_in_thres', inplace=True)

print(thres_df)

graph_data = graph_data.join(thres_df, on='i_fr')

print(f'[{PROCESS}] All stream data loaded from Redis')

# %%
# close the Redis connection
r.close()

gdf = graph_data

# shift ts_end (fr and thres) up by one iteration
gdf['ts_end_fr'][:-5] = gdf['ts_end_fr'][5:]
gdf['ts_end_thres'][:-5] = gdf['ts_end_thres'][5:]

# %%
# Calculate position
gdf['mouse_pos_x'] = np.cumsum(gdf['mouse_vel_x'])
gdf['mouse_pos_y'] = np.cumsum(gdf['mouse_vel_y'])

gdf.dropna(inplace=True)
gdf.drop(gdf.tail(1000).index,
        inplace = True)


#i_max = (gdf['i_thres'].iloc[-1]+1) - (gdf['i_thres'].iloc[-1]+1)%5
#print(i_max)

print(gdf)

max_v_x = gdf['mouse_vel_x'].max()
max_v_y = gdf['mouse_vel_y'].max()

print(f'[{PROCESS}] Max. mouse x/y-velocities: {max_v_x}/{max_v_y}')


# %%
######################
### Generate plots ###
######################

FORMAT = 'png'
date_str = datetime.now().strftime(r'%y%m%dT%H%M')
data_id = date_str

# %%
# save the data to a pickle file
#with open(f'{date_str}_graph_data.pkl', 'wb') as f:
#    pickle.dump(graph_data, f)

# %%
# Plot mouse position
plt.figure(figsize=(8, 8))
plt.title('Mouse Position')
plt.plot(gdf[f'mouse_pos_x'], gdf[f'mouse_pos_y'])
plt.ylabel(f'position')
plt.tight_layout()
plt.savefig(f'{data_id}_mouse_position.{FORMAT}')

# %%
# Plot simulated data

thresholds = np.stack(gdf['thresholds'])
#thresholds2 = np.stack(gdf['thresholds2'])
#thresholds = np.hstack((thresholds1, thresholds2))

fig, axes = plt.subplots(ncols=1, nrows=3, figsize=(8, 12), sharex=True)
# axes[0].set_title('Mouse Position')
# for i, dim in enumerate(['x', 'y']):
#     axes[0].plot(gdf['i_thres'], gdf[f'mouse_pos_{dim}'], label=f'{dim}-position')
# axes[0].set_ylabel(f'{dim} position')
# axes[0].set_title('Mouse Velocity')
axes[0].set_title('Mouse Velocity')
for i, dim in enumerate(['x', 'y']):
    axes[0].plot(gdf['i_thres'], gdf[f'mouse_vel_{dim}'], label=f'{dim}-velocity')
axes[0].legend()
axes[1].set_title('Simulated Firing Rates')
axes[1].imshow(np.stack(gdf['rates']).T, aspect='auto', interpolation=None)
axes[1].set_ylabel('channels')
axes[2].set_title('Simulated Spikes')
axes[2].imshow(1-thresholds.T, aspect='auto',vmin=0,vmax=1,cmap='gray', interpolation=None)
# add second array
axes[2].set_ylabel('channels')
axes[2].set_xlabel('sample (ms) #')
plt.tight_layout()
plt.savefig(f'{data_id}_simulated_data.{FORMAT}')

#total_spikes = np.sum(thresholds, axis=0)/(thresholds.shape[0]/1000)
#print(total_spikes)

# %%
# Plot sample firing rates and spikes
N_channels = 4
T = 1000
fig, axes = plt.subplots(ncols=1, nrows=3, figsize=(8, 12), sharex=True)
axes[0].set_title('Mouse Velocity')
for i, dim in enumerate(['x', 'y']):
    axes[0].plot(gdf['i_thres'].to_numpy()[:T], gdf[f'mouse_vel_{dim}'].to_numpy()[:T], label=f'{dim}-velocity')
axes[1].set_title('Sample firing rates')
rates = np.stack(gdf['rates']).T
for i in range(0, N_channels):
    axes[1].plot(gdf['i_thres'].to_numpy()[:T], rates[i,:T], label=f'Channel {i}')
axes[1].set_ylabel(f'Firing rates [Hz]')
axes[1].legend()
axes[2].set_title('Sample spikes')
thresholds = np.stack(gdf['thresholds']).T
for i in range(0, N_channels):
    axes[2].plot(gdf['i_thres'].to_numpy()[:T], 0.9*thresholds[i,:T]+i, label=f'Channel {i}')
axes[2].set_ylabel(f'Spikes')
axes[2].set_xlabel('sample (ms) #')
plt.tight_layout()
plt.savefig(f'{data_id}_sample_data.{FORMAT}')

# %%
# Plot latent subspaces for generating firing rates
N_channels = 4
dim_prep = 2
dim_move = 2
fig, axes = plt.subplots(ncols=1, nrows=3, figsize=(8, 12), sharex=True)

axes[0].set_title('Preparatory subspace')
rates = np.stack(gdf['prep_subspace']).T
for i in range(0, dim_prep):
    axes[0].plot(gdf['i_thres'], rates[i,:], label=f'Dim {i}')
axes[0].set_ylabel(f'Subspace activity [AU]')
axes[0].legend()

axes[1].set_title('Movement subspace')
rates = np.stack(gdf['move_subspace']).T
for i in range(0, dim_move):
    axes[1].plot(gdf['i_thres'], rates[i,:], label=f'Dim {i}')
axes[1].set_ylabel(f'Subspace activity [AU]')
axes[1].legend()

axes[2].set_title('Sample firing rates')
rates = np.stack(gdf['rates']).T
for i in range(0, N_channels):
    axes[2].plot(gdf['i_thres'], rates[i,:], label=f'Channel {i}')
axes[2].set_ylabel(f'Firing rates [Hz]')
axes[2].legend()

i_move_start = []
i_target_change = []

i_thres = np.stack(gdf['i_thres'])
moving = np.stack(gdf['moving'])
target_state = np.stack(gdf['target_state'])
t_t = np.stack(gdf['t_t'])

for i in i_thres[1:-1]:
    if moving[int(i)] != moving[int(i)-1] and moving[int(i)] == 1:
        i_move_start.append(i)
#axes[0].vlines(i_move_start, ymin=-1, ymax=1, colors='b', linestyles='dashed')
axes[1].vlines(i_move_start, ymin=-1, ymax=1, colors='b', linestyles='dashed')
axes[2].vlines(i_move_start, ymin=0, ymax=100, colors='b', linestyles='dashed')

for i in i_thres[1:-1]:
    if (target_state[int(i)] != target_state[int(i)-1] and 
            ((target_state[int(i)] == 1) or (target_state[int(i)] == 2 and target_state[int(i)-1] != 1))):
        i_target_change.append(i)
axes[0].vlines(i_target_change, ymin=-1, ymax=1, colors='g', linestyles='dashed')
axes[2].vlines(i_target_change, ymin=0, ymax=100, colors='g', linestyles='dashed')

plt.tight_layout()
plt.savefig(f'{data_id}_subspace_activity.{FORMAT}')

#print(rates[:,0])

#print(total_spikes - rates[:,0])

# Plot simulator states
fig, axes = plt.subplots(ncols=1, nrows=3, figsize=(8, 12), sharex=True)

axes[0].set_title('Target state')
axes[0].plot(gdf['i_thres'], target_state)
axes[0].set_ylabel(f'Target state')

axes[1].set_title('Moving state')
axes[1].plot(gdf['i_thres'], moving)
axes[1].set_ylabel(f'Move intention')

axes[2].set_title('Prep activity on')
axes[2].plot(gdf['i_thres'], t_t)
axes[2].set_ylabel(f'Prep activity on')

plt.tight_layout()
plt.savefig(f'{data_id}_simulator_state.{FORMAT}')

# %%
# Inter-Sample Interval
isi_fields_5ms = ['ts_mouse', 'ts_fr']
isi_labels_5ms = ['mouse_vel', 'simulator2D']
isi_values_5ms = gdf[isi_fields_5ms].diff().values[::5, :] * 1e3
isi_fields_1ms = ['ts_thres']
isi_labels_1ms = ['spike_gen_1ms']
isi_values_1ms = gdf[isi_fields_1ms].diff().values[:, :] * 1e3
'''
plt.figure()
plt.violinplot(isi_values)
plt.ylabel('Inter-sample interval (ms)')
xticks = np.arange(len(isi_fields)) + 1
plt.xticks(xticks, isi_fields)
plt.tight_layout()
plt.savefig(f'{data_id}_per_node_isi_violin.pdf')
'''

# Plot Inter-Sample Interval over time
nplots = isi_values_5ms.shape[1] + isi_values_1ms.shape[1]
fig, axes = plt.subplots(ncols=1, nrows=nplots, figsize=(8, 12), sharey=True)
for i in range(0,2):
    axes[i].set_title(f'{isi_labels_5ms[i]}\n'
        f'Mean: {np.nanmean(isi_values_5ms[:, i]) :.4f} ms -- '
        f'Range: {np.nanmin(isi_values_5ms[:, i]) :.4f}-{np.nanmax(isi_values_5ms[:, i]) :.4f} ms')
    axes[i].plot(isi_values_5ms[:, i])
    axes[i].set_ylabel('Inter-sample interval (ms)')
for i in range(2,3):
    axes[i].set_title(f'{isi_labels_1ms[i-2]}\n'
        f'Mean: {np.nanmean(isi_values_1ms[:, i-2]) :.4f} ms -- '
        f'Range: {np.nanmin(isi_values_1ms[:, i-2]) :.4f}-{np.nanmax(isi_values_1ms[:, i-2]) :.4f} ms')
    axes[i].plot(isi_values_1ms[:, i-2])
    axes[i].set_ylabel('Inter-sample interval (ms)')
axes[-1].set_xlabel('Sample #')    
axes[-1].set_ylim([0, None])
plt.tight_layout()
plt.savefig(f'{data_id}_per_node_isi_over_time.{FORMAT}')





thresholds = np.stack(gdf['thresholds']).T
#thresholds = np.stack(gdf['thresholds2']).T
#hresholds = np.vstack((thresholds1, thresholds2))

N_channels = 4
N_arrays = 4
N_samples = 200 # 100 ms
fig, axes = plt.subplots(ncols=1, nrows=N_arrays, figsize=(30, N_arrays*3), sharex=True)
N_per_array = int(n_neurons/N_arrays)
#print(f'Scale continuous: {scale_continuous1}')
#print(f'Neurons per array: {N_per_array}')
for r in range(0,N_arrays):
    axes[r].set_title(f'Array {r+1}')
    for i in range(0, N_channels):
        axes[r].plot(thresholds[int(r*N_per_array+i), :N_samples]+i*5, label=f'Channel {int(r*N_per_array+i)}')
    axes[r].set_ylabel('Spike counts per ms')
    axes[r].legend()
axes[-1].set_xlabel('sample #')
plt.tight_layout()
plt.savefig(f'{data_id}_thresholds_data.{FORMAT}')

N_channels = 4
N_arrays = 4
N_samples = -1 # 100 ms
fig, axes = plt.subplots(ncols=1, nrows=N_arrays, figsize=(30, N_arrays*3), sharex=True)
N_per_array = int(n_neurons/N_arrays)
#print(f'Scale continuous: {scale_continuous1}')
#print(f'Neurons per array: {N_per_array}')
for r in range(0,N_arrays):
    axes[r].set_title(f'Array {r+1}')
    for i in range(0, N_channels):
        axes[r].plot(rates[int(r*N_per_array+i), :N_samples], label=f'Channel {int(r*N_per_array+i)}')
    axes[r].set_ylabel('Firing rates (Hz)')
    axes[r].legend()
axes[-1].set_xlabel('sample #')
plt.tight_layout()
plt.savefig(f'{data_id}_rates_data.{FORMAT}')

