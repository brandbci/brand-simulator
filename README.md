# brand-simulator
BRAND module for neural simulator

![image](https://user-images.githubusercontent.com/11467761/184686652-5224d80c-2a1d-45ed-bfe8-79aec0d3c339.png)

## How to use

The simulator is run using [BRAND](https://github.com/snel-repo/realtime_rig_dev/tree/dev). This will require first starting the BRAND supervisor on you simulator machine:
```
supervisor
```
If you are running the simulator in the same machine as another graph (local version) you should assign a different port to the supervisor instance that will run the simulator:
```
supervisor -p <port> (e.g 6380)
```

Once the supervisor has started, you need to send it a `startGraph` command. A simple way to do this is using `redis-cli`. On a new terminal:
```
redis-cli -p <port> (e.g 6380)
XADD supervisor_ipstream * commands startGraph file <path_to_simulator_graph_yaml>
```

The simulator will start after this, and will start outputting data after some seconds.
